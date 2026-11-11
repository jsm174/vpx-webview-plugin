// license:GPLv3+

#include "TableBridge.h"

#include "Pages.h"
#include "Plugin.h"

#include <cmath>
#include <deque>
#include <mutex>
#include <random>

namespace WebView {

namespace {

std::string eventHandler;

std::mutex eventsMutex;
struct PendingEvent
{
   std::string page;
   std::string name;
   std::string data;
};
std::deque<PendingEvent> events;

struct TableRequest
{
   uint32_t page;
   int request;
   std::string kind;
   std::string part;
   std::string member;
   CefRefPtr<CefListValue> args;
};
std::deque<TableRequest> tableRequests;
int bridgeToken = 0;
struct
{
   uint32_t page;
   int request;
   bool waiting = false;
} activeRequest;

// Installed into the table script on the first page request. WebView__Json turns any VBScript value into JSON, so page
// requests can be answered through a single string argument
constexpr const char* kTableHelper = R"VBS(
Dim WebView__, WebView__Value
Set WebView__ = CreateObject("WebView.Bridge")
Function WebView__Json(v)
	Dim i, s
	If IsArray(v) Then
		s = "["
		For i = LBound(v) To UBound(v)
			If i > LBound(v) Then s = s & ","
			s = s & WebView__Json(v(i))
		Next
		WebView__Json = s & "]"
		Exit Function
	End If
	Select Case VarType(v)
		Case 0, 1
			WebView__Json = "null"
		Case 11
			If v Then WebView__Json = "true" Else WebView__Json = "false"
		Case 2, 3, 4, 5, 6, 14, 16, 17, 18, 19, 20, 21
			WebView__Json = Replace(CStr(v), ",", ".")
		Case 8
			s = Replace(v, "\", "\\")
			s = Replace(s, Chr(34), "\" & Chr(34))
			s = Replace(s, vbCr, "\r")
			s = Replace(s, vbLf, "\n")
			s = Replace(s, vbTab, "\t")
			WebView__Json = Chr(34) & s & Chr(34)
		Case Else
			WebView__Json = "null"
	End Select
End Function
)VBS";

void MSGPIAPI DrainEvents(void*)
{
   std::deque<PendingEvent> batch;
   {
      std::lock_guard lock(eventsMutex);
      batch.swap(events);
   }
   if (eventHandler.empty() || !vpxApi)
      return;
   for (const auto& e : batch)
      vpxApi->RunScript(std::format("{} {}, {}, {}", eventHandler, VbsString(e.page), VbsString(e.name), VbsString(e.data)).c_str());
}

void ReplyToPage(uint32_t page, int request, bool ok, const std::string& payload)
{
   EvalInPage(page, std::format("window.vpx && vpx._reply({}, {}, {})", request, ok ? "true" : "false", JsonEscape(payload)));
}

bool ToVbsPath(const std::string& path, std::string& out)
{
   out.clear();
   size_t start = 0;
   while (true)
   {
      const size_t dot = path.find('.', start);
      const std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      if (!IsIdentifier(name))
         return false;
      out += out.empty() ? name : "." + name;
      if (dot == std::string::npos)
         return true;
      start = dot + 1;
   }
}

bool ToVbsLiteral(const CefRefPtr<CefValue>& value, std::string& out)
{
   switch (value->GetType())
   {
   case VTYPE_NULL:
   case VTYPE_INVALID: out = "Empty"; return true;
   case VTYPE_BOOL: out = value->GetBool() ? "True" : "False"; return true;
   case VTYPE_INT: out = std::to_string(value->GetInt()); return true;
   case VTYPE_DOUBLE:
   {
      const double d = value->GetDouble();
      if (!std::isfinite(d))
         return false;
      out = std::format("{}", d);
      return true;
   }
   case VTYPE_STRING: out = VbsString(value->GetString().ToString()); return true;
   case VTYPE_LIST:
   {
      const auto list = value->GetList();
      out = "Array(";
      for (size_t i = 0; i < list->GetSize(); i++)
      {
         std::string item;
         if (!ToVbsLiteral(list->GetValue(i), item))
            return false;
         out += (i ? ", " : "") + item;
      }
      out += ')';
      return true;
   }
   default: return false;
   }
}

bool BuildStatement(const TableRequest& req, std::string& statement, std::string& error)
{
   if (req.kind == "watch")
   {
      std::string list;
      for (size_t i = 0; i < req.args->GetSize(); i++)
      {
         const std::string path = req.args->GetString(i).ToString();
         std::string vbs;
         if (!ToVbsPath(path, vbs))
         {
            error = "Invalid name '" + path + "'";
            return false;
         }
         list += (i ? ", " : "") + vbs;
      }
      statement = "WebView__Value = WebView__Json(Array(" + list + "))";
      return true;
   }

   const std::string path = req.part.empty() ? req.member : req.part + "." + req.member;
   std::string target;
   if (!ToVbsPath(path, target))
   {
      error = "Invalid name '" + path + "'";
      return false;
   }
   std::string args;
   for (size_t i = 0; i < req.args->GetSize(); i++)
   {
      std::string literal;
      if (!ToVbsLiteral(req.args->GetValue(i), literal))
      {
         error = "Unsupported value for " + target;
         return false;
      }
      args += (i ? ", " : "") + literal;
   }
   if (req.kind == "get")
      statement = "WebView__Value = WebView__Json(" + target + ")";
   else if (req.kind == "set")
      statement = target + " = " + args + "\nWebView__Value = \"null\"";
   else if (req.kind == "call")
      statement = "WebView__Value = WebView__Json(" + target + "(" + args + "))";
   else
   {
      error = "Unknown request " + req.kind;
      return false;
   }
   return true;
}

void MSGPIAPI DrainTableRequests(void*)
{
   std::deque<TableRequest> batch;
   batch.swap(tableRequests);
   for (const auto& req : batch)
   {
      if (!vpxApi)
      {
         ReplyToPage(req.page, req.request, false, "No game is running");
         continue;
      }
      std::string statement;
      std::string error;
      if (!BuildStatement(req, statement, error))
      {
         ReplyToPage(req.page, req.request, false, error);
         continue;
      }
      if (bridgeToken == 0)
      {
         std::random_device random;
         bridgeToken = static_cast<int>(random() & 0x3FFFFFFF) + 1;
         vpxApi->RunScript(kTableHelper);
      }
      const std::string script = std::format("On Error Resume Next\n{3}\nIf Err.Number <> 0 Then\nWebView__.Fail {0}, {1}, {2}, Err.Description & \" (\" & Err.Number & \")\"\nElse\n"
                                             "WebView__.Reply {0}, {1}, {2}, WebView__Value\nEnd If\nErr.Clear\nOn Error Goto 0\n",
         bridgeToken, req.page, req.request, statement);
      activeRequest = { req.page, req.request, true };
      vpxApi->RunScript(script.c_str());
      if (activeRequest.waiting)
      {
         activeRequest.waiting = false;
         ReplyToPage(req.page, req.request, false, "VBScript could not run: " + statement);
      }
   }
}

}

void QueueEvent(const std::string& page, std::string name, std::string data)
{
   {
      std::lock_guard lock(eventsMutex);
      events.push_back({ page, std::move(name), std::move(data) });
   }
   if (msgApi)
      msgApi->RunOnMainThread(endpointId, kQueued, DrainEvents, nullptr);
}

void SetEventHandler(const std::string& name)
{
   if (name.empty() || IsIdentifier(name))
      eventHandler = name;
   else
      LOGE(std::format("EventHandler must be the name of a Sub, got '{}'", name));
}

std::string GetEventHandler() { return eventHandler; }

void QueueTableRequest(uint32_t pageId, int request, bool allowed, std::string kind, std::string part, std::string member, CefRefPtr<CefListValue> args)
{
   if (!allowed)
   {
      ReplyToPage(pageId, request, false, "Table access is only available to pages loaded from the table folder");
      return;
   }
   tableRequests.push_back({ pageId, request, std::move(kind), std::move(part), std::move(member), std::move(args) });
   if (msgApi)
      msgApi->RunOnMainThread(endpointId, kQueued, DrainTableRequests, nullptr);
}

void BridgeAnswer(int token, int pageId, int request, bool ok, const std::string& payload)
{
   if (token != bridgeToken || bridgeToken == 0)
      return;
   if (activeRequest.waiting && activeRequest.page == static_cast<uint32_t>(pageId) && activeRequest.request == request)
      activeRequest.waiting = false;
   ReplyToPage(static_cast<uint32_t>(pageId), request, ok, payload);
}

void ResetBridge()
{
   eventHandler.clear();
   tableRequests.clear();
   bridgeToken = 0;
   activeRequest.waiting = false;
   std::lock_guard lock(eventsMutex);
   events.clear();
}

}
