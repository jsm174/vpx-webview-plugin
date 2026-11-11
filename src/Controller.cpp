// license:GPLv3+

#include "Controller.h"

#include "CefHost.h"
#include "Pages.h"
#include "Plugin.h"
#include "TableBridge.h"
#include "WebViewVersion.h"

namespace WebView {

using std::string;

namespace {

// WebView.Controller: what table scripts use
class Controller final
{
public:
   PSC_IMPLEMENT_REFCOUNT()

   string GetVersion() const { return WEBVIEW_PLUGIN_VERSION; }
   bool GetRunning() const { return IsCefRunning(); }
   string GetEventHandler() const { return WebView::GetEventHandler(); }
   void SetEventHandler(const string& name) { WebView::SetEventHandler(name); }

   void Screen(int id, int width, int height) { CreateScreen(id, width, height); }
   void CloseScreen(int id) { WebView::CloseScreen(id); }
   void ShowScreen(int id, const string& windows) { WebView::ShowScreen(id, windows); }

   void Open(const string& page, const string& url) { OpenPage(page, url); }
   void Close(const string& page) { ClosePage(page); }
   void Place(const string& page, int screen, int x, int y, int width, int height) { PlacePage(page, screen, x, y, width, height); }
   void SetLayer(const string& page, int layer) { SetPageLayer(page, layer); }
   void SetVolume(const string& page, float volume) { SetPageVolume(page, volume); }
   void SetFrameRate(const string& page, int fps) { SetPageFrameRate(page, fps); }
   void Eval(const string& page, const string& js) { EvalInPage(page, js); }
   void Send(const string& page, const string& name, const string& data)
   {
      EvalInPage(page, std::format("window.vpx && vpx._dispatch({}, {})", JsonEscape(name), JsonEscape(data)));
   }
};

// WebView.Bridge: only used by the helper script the plugin injects to answer vpx.table requests
class Bridge final
{
public:
   PSC_IMPLEMENT_REFCOUNT()

   void Reply(int token, int page, int request, const string& json) { BridgeAnswer(token, page, request, true, json); }
   void Fail(int token, int page, int request, const string& error) { BridgeAnswer(token, page, request, false, error); }
};

}

PSC_CLASS_START(WebView_Controller, Controller)
   PSC_FUNCTION0(string, GetVersion)
   PSC_PROP_R(bool, Running)
   PSC_PROP_RW(string, EventHandler)
   PSC_FUNCTION3(void, Screen, int, int, int)
   PSC_FUNCTION1(void, CloseScreen, int)
   PSC_FUNCTION2(void, ShowScreen, int, string)
   PSC_FUNCTION2(void, Open, string, string)
   PSC_FUNCTION1(void, Close, string)
   PSC_FUNCTION6(void, Place, string, int, int, int, int, int)
   PSC_FUNCTION2(void, SetLayer, string, int)
   PSC_FUNCTION2(void, SetVolume, string, float)
   PSC_FUNCTION2(void, SetFrameRate, string, int)
   PSC_FUNCTION2(void, Eval, string, string)
   PSC_FUNCTION3(void, Send, string, string, string)
PSC_CLASS_END()

PSC_CLASS_START(WebView_Bridge, Bridge)
   PSC_FUNCTION4(void, Reply, int, int, int, string)
   PSC_FUNCTION4(void, Fail, int, int, int, string)
PSC_CLASS_END()

void RegisterScriptClasses()
{
   if (!scriptApi)
      return;
   RegisterWebView_Controller([](ScriptClassDef* scd) { scriptApi->RegisterScriptClass(scd); });
   RegisterWebView_Bridge([](ScriptClassDef* scd) { scriptApi->RegisterScriptClass(scd); });
   WebView_Controller_SCD->CreateObject = []() { return static_cast<void*>(new Controller()); };
   WebView_Bridge_SCD->CreateObject = []() { return static_cast<void*>(new Bridge()); };
   scriptApi->SubmitTypeLibrary(endpointId);
   scriptApi->SetCOMObjectOverride("WebView.Controller", WebView_Controller_SCD);
   scriptApi->SetCOMObjectOverride("WebView.Bridge", WebView_Bridge_SCD);
}

void UnregisterScriptClasses()
{
   if (!scriptApi)
      return;
   scriptApi->SetCOMObjectOverride("WebView.Controller", nullptr);
   scriptApi->SetCOMObjectOverride("WebView.Bridge", nullptr);
   UnregisterWebView_Controller([](ScriptClassDef* scd) { scriptApi->UnregisterScriptClass(scd); });
   UnregisterWebView_Bridge([](ScriptClassDef* scd) { scriptApi->UnregisterScriptClass(scd); });
}

}
