// license:GPLv3+

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#ifdef __APPLE__
#include "include/cef_sandbox_mac.h"
#include "include/wrapper/cef_library_loader.h"
#include <climits>
#include <cstdlib>
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include "Scheme.h"

#include <filesystem>
#include <map>
#include <string>

namespace {

constexpr const char* kBridge = R"JS(
(function (emit, request, name) {
  const handlers = new Map()
  const pending = new Map()
  let nextRequest = 1

  const send = (kind, part, member, args) => new Promise((resolve, reject) => {
    const req = nextRequest++
    pending.set(req, { resolve, reject })
    request(req, kind, part, member, args)
  })

  const fail = (promise) => promise.catch((err) => console.error('vpx.table: ' + err.message))

  const part = (name) => new Proxy(function () {}, {
    apply(_, __, args) {
      return send('call', '', name, args)
    },
    get(_, member) {
      if (typeof member !== 'string') return undefined
      if (member === 'then') return (resolve, reject) => send('get', '', name, []).then(resolve, reject)
      const call = (...args) => send('call', name, member, args)
      call.then = (resolve, reject) => send('get', name, member, []).then(resolve, reject)
      call.catch = (reject) => call.then(undefined, reject)
      return call
    },
    set(_, member, value) {
      fail(send('set', name, String(member), [value]))
      return true
    }
  })

  const table = new Proxy({}, {
    get(_, name) {
      return typeof name === 'string' && name !== 'then' ? part(name) : undefined
    },
    set(_, name, value) {
      fail(send('set', '', String(name), [value]))
      return true
    }
  })

  const watch = (paths, cb) => {
    const list = Array.isArray(paths) ? paths : [paths]
    let last
    let stopped = false
    let busy = false
    const tick = async () => {
      if (stopped) return
      if (!busy) {
        busy = true
        try {
          const values = await send('watch', '', '', list.map(String))
          const json = JSON.stringify(values)
          if (json !== last) {
            last = json
            cb(Array.isArray(paths) ? values : values[0])
          }
        } catch (err) {
          stopped = true
          console.error('vpx.watch stopped: ' + err.message)
        }
        busy = false
      }
      requestAnimationFrame(tick)
    }
    requestAnimationFrame(tick)
    return () => { stopped = true }
  }

  const vpx = {
    name,
    table,
    watch,
    on(name, cb) {
      if (!handlers.has(name)) handlers.set(name, new Set())
      handlers.get(name).add(cb)
      return () => handlers.get(name).delete(cb)
    },
    emit(name, data) {
      emit(String(name), data === undefined ? '' : typeof data === 'string' ? data : JSON.stringify(data))
    }
  }
  Object.defineProperty(vpx, '_dispatch', {
    value(name, data) {
      for (const key of [name, '*']) {
        for (const cb of handlers.get(key) || []) {
          try {
            cb(data, name)
          } catch (err) {
            console.error(err)
          }
        }
      }
    }
  })
  Object.defineProperty(vpx, '_reply', {
    value(req, ok, payload) {
      const entry = pending.get(req)
      if (!entry) return
      pending.delete(req)
      if (!ok) return entry.reject(new Error(payload))
      try {
        entry.resolve(JSON.parse(payload))
      } catch (err) {
        entry.reject(new Error('Unreadable table value: ' + payload))
      }
    }
  })
  Object.defineProperty(window, 'vpx', { value: Object.freeze(vpx) })
})
)JS";

class EmitHandler final : public CefV8Handler
{
public:
   bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>&, CefString&) override
   {
      CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
      if (!context || arguments.size() < 2)
         return true;
      CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("vpx.emit");
      message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
      message->GetArgumentList()->SetString(1, arguments[1]->GetStringValue());
      context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
      return true;
   }

private:
   IMPLEMENT_REFCOUNTING(EmitHandler);
};

CefRefPtr<CefValue> ToValue(const CefRefPtr<CefV8Value>& v)
{
   CefRefPtr<CefValue> out = CefValue::Create();
   if (v->IsBool())
      out->SetBool(v->GetBoolValue());
   else if (v->IsInt())
      out->SetInt(v->GetIntValue());
   else if (v->IsDouble())
      out->SetDouble(v->GetDoubleValue());
   else if (v->IsString())
      out->SetString(v->GetStringValue());
   else if (v->IsArray())
   {
      CefRefPtr<CefListValue> list = CefListValue::Create();
      for (int i = 0; i < v->GetArrayLength(); i++)
         list->SetValue(i, ToValue(v->GetValue(i)));
      out->SetList(list);
   }
   else
      out->SetNull();
   return out;
}

class RequestHandler final : public CefV8Handler
{
public:
   bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>&, CefString&) override
   {
      CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
      if (!context || arguments.size() < 5)
         return true;
      CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("vpx.table");
      CefRefPtr<CefListValue> args = message->GetArgumentList();
      args->SetInt(0, arguments[0]->GetIntValue());
      args->SetString(1, arguments[1]->GetStringValue());
      args->SetString(2, arguments[2]->GetStringValue());
      args->SetString(3, arguments[3]->GetStringValue());
      args->SetValue(4, ToValue(arguments[4]));
      context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
      return true;
   }

private:
   IMPLEMENT_REFCOUNTING(RequestHandler);
};

class HelperApp final
   : public CefApp
   , public CefRenderProcessHandler
{
public:
   CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

   void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { WebView::RegisterTableScheme(registrar); }

   void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra) override
   {
      if (extra)
         m_pages[browser->GetIdentifier()] = extra->GetString("name").ToString();
   }

   void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override { m_pages.erase(browser->GetIdentifier()); }

   void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
   {
      if (!frame->IsMain())
         return;
      const auto page = m_pages.find(browser->GetIdentifier());
      if (page == m_pages.end())
         return;
      CefRefPtr<CefV8Value> install;
      CefRefPtr<CefV8Exception> exception;
      if (!context->Eval(kBridge, "vpx://bridge", 1, install, exception) || !install || !install->IsFunction())
         return;
      CefV8ValueList args {
         CefV8Value::CreateFunction("emit", new EmitHandler()),
         CefV8Value::CreateFunction("request", new RequestHandler()),
         CefV8Value::CreateString(page->second),
      };
      install->ExecuteFunction(nullptr, args);
   }

private:
   std::map<int, std::string> m_pages;

   IMPLEMENT_REFCOUNTING(HelperApp);
};

}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
   CefMainArgs mainArgs(instance);
   return CefExecuteProcess(mainArgs, new HelperApp(), nullptr);
}
#else
int main(int argc, char* argv[])
{
#ifdef __APPLE__
   CefScopedSandboxContext sandbox;
   if (!sandbox.Initialize(argc, argv))
      return 1;
   // Helper bundles sit next to the framework in the plugin folder: <plugin>/WebView Helper.app/Contents/MacOS/WebView Helper
   char path[PATH_MAX];
   uint32_t size = sizeof(path);
   char resolved[PATH_MAX];
   if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, resolved))
      return 1;
   const std::filesystem::path framework = std::filesystem::path(resolved).parent_path().parent_path().parent_path().parent_path() / "Chromium Embedded Framework.framework" / "Chromium Embedded Framework";
   if (!cef_load_library(framework.c_str()))
      return 1;
   CefMainArgs mainArgs(argc, argv);
#else
   CefMainArgs mainArgs(argc, argv);
#endif
   return CefExecuteProcess(mainArgs, new HelperApp(), nullptr);
}
#endif
