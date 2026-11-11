// license:GPLv3+

#include "CefHost.h"

#include "Plugin.h"
#include "Scheme.h"

#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/cef_version.h"
#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#endif

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace WebView {

#ifdef __APPLE__
void PrepareApplication();
#endif

namespace {

enum class CefState
{
   Uninitialized,
   Running,
   Failed
};
CefState cefState = CefState::Uninitialized;
std::atomic<bool> pumpEnabled { false };
std::atomic<bool> pumpQueued { false };
std::atomic<uintptr_t> pumpGeneration { 0 };
bool inPump = false;

std::mutex tableRootMutex;
std::filesystem::path tableRoot;

void MSGPIAPI Pump(void*)
{
   pumpQueued = false;
   if (!pumpEnabled || inPump)
      return;
   inPump = true;
   CefDoMessageLoopWork();
   inPump = false;
}

void SchedulePump(double delay)
{
   if (!pumpEnabled || !msgApi)
      return;
   if (delay <= kQueued)
   {
      if (pumpQueued.exchange(true))
         return;
      delay = kQueued;
   }
   msgApi->RunOnMainThread(endpointId, delay, Pump, nullptr);
}

// CEF asks for pump work through OnScheduleMessagePumpWork, and also needs a regular fallback pump
void MSGPIAPI Heartbeat(void* generation)
{
   if (!pumpEnabled || !msgApi || reinterpret_cast<uintptr_t>(generation) != pumpGeneration)
      return;
   Pump(nullptr);
   msgApi->RunOnMainThread(endpointId, 1. / 60., Heartbeat, generation);
}

void EnablePump()
{
   if (pumpEnabled)
      return;
   pumpEnabled = true;
   Heartbeat(reinterpret_cast<void*>(++pumpGeneration));
}

// Serves one file of the table folder, with range support so that <video> and <audio> can seek
class FileResourceHandler final : public CefResourceHandler
{
public:
   explicit FileResourceHandler(std::filesystem::path path)
      : m_path(std::move(path))
   {
   }

   bool Open(CefRefPtr<CefRequest> request, bool& handleRequest, CefRefPtr<CefCallback>) override
   {
      handleRequest = true;
      std::error_code ec;
      if (m_path.empty() || !std::filesystem::is_regular_file(m_path, ec))
         return true;
      m_file.open(m_path, std::ios::binary);
      if (!m_file)
         return true;
      m_size = static_cast<int64_t>(std::filesystem::file_size(m_path, ec));
      m_found = true;
      m_start = 0;
      m_remaining = m_size;

      const std::string range = request->GetHeaderByName("Range").ToString();
      if (range.starts_with("bytes="))
      {
         const size_t dash = range.find('-', 6);
         if (dash != std::string::npos)
         {
            const std::string first = range.substr(6, dash - 6);
            const std::string last = range.substr(dash + 1);
            int64_t start = 0;
            int64_t end = m_size - 1;
            try
            {
               if (first.empty())
                  start = std::max<int64_t>(0, m_size - std::stoll(last));
               else
               {
                  start = std::stoll(first);
                  if (!last.empty())
                     end = std::min<int64_t>(std::stoll(last), m_size - 1);
               }
            }
            catch (...)
            {
               start = 0;
               end = m_size - 1;
            }
            if (start <= end && start < m_size)
            {
               m_partial = true;
               m_start = start;
               m_remaining = end - start + 1;
               m_file.seekg(start);
            }
         }
      }
      return true;
   }

   void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& responseLength, CefString&) override
   {
      if (!m_found)
      {
         response->SetStatus(404);
         response->SetStatusText("Not Found");
         responseLength = 0;
         return;
      }
      std::string extension = m_path.extension().string();
      if (!extension.empty())
         extension.erase(0, 1);
      const std::string mime = CefGetMimeType(extension).ToString();
      response->SetMimeType(mime.empty() ? "application/octet-stream" : mime);
      response->SetStatus(m_partial ? 206 : 200);
      response->SetStatusText(m_partial ? "Partial Content" : "OK");
      response->SetHeaderByName("Accept-Ranges", "bytes", true);
      response->SetHeaderByName("Cache-Control", "no-cache", true);
      if (m_partial)
         response->SetHeaderByName("Content-Range", std::format("bytes {}-{}/{}", m_start, m_start + m_remaining - 1, m_size), true);
      responseLength = m_remaining;
   }

   bool Read(void* dataOut, int bytesToRead, int& bytesRead, CefRefPtr<CefResourceReadCallback>) override
   {
      bytesRead = 0;
      if (!m_found || m_remaining <= 0)
         return false;
      m_file.read(static_cast<char*>(dataOut), std::min<int64_t>(bytesToRead, m_remaining));
      bytesRead = static_cast<int>(m_file.gcount());
      m_remaining -= bytesRead;
      return bytesRead > 0;
   }

   void Cancel() override { }

private:
   const std::filesystem::path m_path;
   std::ifstream m_file;
   bool m_found = false;
   bool m_partial = false;
   int64_t m_size = 0;
   int64_t m_start = 0;
   int64_t m_remaining = 0;

   IMPLEMENT_REFCOUNTING(FileResourceHandler);
};

class TableSchemeHandlerFactory final : public CefSchemeHandlerFactory
{
public:
   CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&, CefRefPtr<CefRequest> request) override
   {
      std::filesystem::path root;
      {
         std::lock_guard lock(tableRootMutex);
         root = tableRoot;
      }
      CefURLParts parts;
      if (root.empty() || !CefParseURL(request->GetURL(), parts))
         return new FileResourceHandler({});
      const std::string decoded = CefURIDecode(CefString(&parts.path), true,
         static_cast<cef_uri_unescape_rule_t>(UU_SPACES | UU_PATH_SEPARATORS | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS)).ToString();
      const std::u8string relative(decoded.begin() + (decoded.starts_with('/') ? 1 : 0), decoded.end());
      // Symbolic links placed in the table folder are followed on purpose, but the url itself may not climb out of it
      const std::filesystem::path path = (root / std::filesystem::path(relative)).lexically_normal();
      const std::filesystem::path inside = path.lexically_relative(root.lexically_normal());
      if (inside.empty() || *inside.begin() == "..")
      {
         LOGW(std::format("Refused {} (outside of the table folder)", request->GetURL().ToString()));
         return new FileResourceHandler({});
      }
      return new FileResourceHandler(path);
   }

private:
   IMPLEMENT_REFCOUNTING(TableSchemeHandlerFactory);
};

class BrowserApp final
   : public CefApp
   , public CefBrowserProcessHandler
{
public:
   CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

   void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { RegisterTableScheme(registrar); }

   void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> commandLine) override
   {
      commandLine->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
      commandLine->AppendSwitch("enable-unsafe-webgpu");
      commandLine->AppendSwitch("disable-background-timer-throttling");
      commandLine->AppendSwitch("disable-renderer-backgrounding");
#ifdef __APPLE__
      commandLine->AppendSwitch("use-mock-keychain");
#endif
      if (SettingDevTools())
         commandLine->AppendSwitchWithValue("remote-debugging-port", "9222");
   }

   void OnScheduleMessagePumpWork(int64_t delayMs) override { SchedulePump(static_cast<double>(delayMs) / 1000.); }

private:
   IMPLEMENT_REFCOUNTING(BrowserApp);
};

#ifdef __APPLE__
std::filesystem::path BundleDirectory() { return PluginDirectory() / "WebView.app"; }
#endif

std::filesystem::path HelperExecutable()
{
#if defined(_WIN32)
   return PluginDirectory() / "webview-helper.exe";
#elif defined(__APPLE__)
   return BundleDirectory() / "Contents" / "Frameworks" / "WebView Helper.app" / "Contents" / "MacOS" / "WebView Helper";
#else
   return PluginDirectory() / "webview-helper";
#endif
}

std::string PercentEncodePath(const std::u8string& path)
{
   std::string out;
   for (const char8_t c8 : path)
   {
      const auto c = static_cast<unsigned char>(c8);
      if (c <= 0x20 || c == '%' || c == '#' || c == '?' || c >= 0x7F)
         out += std::format("%{:02X}", c);
      else
         out += static_cast<char>(c);
   }
   return out;
}

bool IsExternalScheme(const std::string& url)
{
   for (const char* scheme : { "http:", "https:", "ws:", "wss:", "ftp:", "file:" })
      if (url.starts_with(scheme))
         return true;
   return false;
}

}

bool IsCefRunning() { return cefState == CefState::Running; }

void PauseCef() { pumpEnabled = false; }

bool StartCef()
{
   if (cefState == CefState::Running)
   {
      EnablePump();
      return true;
   }
   if (cefState == CefState::Failed)
      return false;
   cefState = CefState::Failed;

#ifdef __APPLE__
   const std::filesystem::path framework = BundleDirectory() / "Contents" / "Frameworks" / "Chromium Embedded Framework.framework";
   if (!cef_load_library((framework / "Chromium Embedded Framework").string().c_str()))
   {
      LOGE(std::format("Failed to load {}", framework.string()));
      return false;
   }
   PrepareApplication();
#endif

   VPXInfo info {};
   vpxApi->GetVpxInfo(&info);
   const std::filesystem::path cacheDir = PluginStrings::PathFromNative(info.prefPath) / "webview";

   CefSettings settings;
   settings.no_sandbox = true;
   settings.external_message_pump = true;
   settings.windowless_rendering_enabled = true;
   settings.log_severity = LOGSEVERITY_WARNING;
   settings.background_color = 0;
   CefString(&settings.browser_subprocess_path).FromString(HelperExecutable().string());
   CefString(&settings.root_cache_path).FromString(cacheDir.string());
   CefString(&settings.log_file).FromString((cacheDir / "cef.log").string());
#ifdef __APPLE__
   CefString(&settings.framework_dir_path).FromString(framework.string());
   CefString(&settings.main_bundle_path).FromString(BundleDirectory().string());
#endif

#ifdef _WIN32
   CefMainArgs args(GetModuleHandle(nullptr));
#else
   static char arg0[] = "VPinballX";
   static char* argv[] = { arg0, nullptr };
   CefMainArgs args(1, argv);
#endif
   pumpEnabled = true;
   if (!CefInitialize(args, settings, new BrowserApp(), nullptr))
   {
      pumpEnabled = false;
      LOGE(std::format("CEF failed to initialize (exit code {})", CefGetExitCode()));
      return false;
   }
   CefRegisterSchemeHandlerFactory("vpx", "table", new TableSchemeHandlerFactory());
   cefState = CefState::Running;
   pumpEnabled = false;
   EnablePump();
   LOGI(std::format("CEF {} started (Chromium {}.{}.{}.{})", CEF_VERSION, CHROME_VERSION_MAJOR, CHROME_VERSION_MINOR, CHROME_VERSION_BUILD, CHROME_VERSION_PATCH));
   return true;
}

void PumpCefUntil(const std::function<bool()>& done, double timeoutSeconds)
{
   if (cefState != CefState::Running || inPump)
      return;
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
   while (!done() && std::chrono::steady_clock::now() < deadline)
   {
      inPump = true;
      CefDoMessageLoopWork();
      inPump = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
}

void SetTableRoot(const std::filesystem::path& root)
{
   std::lock_guard lock(tableRootMutex);
   tableRoot = root;
}

bool ResolveUrl(const std::string& url, std::string& resolved)
{
   const bool hasScheme = url.find("://") != std::string::npos || url.starts_with("data:") || url.starts_with("about:");
   const bool isAbsolutePath = !hasScheme && std::filesystem::path(std::u8string(url.begin(), url.end())).is_absolute();
   if (!hasScheme && !isAbsolutePath)
   {
      const size_t split = url.find_first_of("?#");
      std::string path = url.substr(0, split);
      std::ranges::replace(path, '\\', '/');
      resolved = kTableOrigin + PercentEncodePath(std::u8string(path.begin(), path.end())) + (split == std::string::npos ? std::string() : url.substr(split));
      return true;
   }
   if (isAbsolutePath)
   {
      const size_t split = url.find_first_of("?#");
      const auto generic = std::filesystem::path(std::u8string(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(std::min(split, url.size())))).generic_u8string();
      resolved = "file://"s + (generic.starts_with(u8'/') ? "" : "/") + PercentEncodePath(generic) + (split == std::string::npos ? std::string() : url.substr(split));
   }
   else
      resolved = url;
   if (!IsUrlAllowed(resolved))
   {
      LOGE(std::format("Refused {}: only pages in the table folder are allowed. Set AllowExternalUrls = 1 in [Plugin.WebView] to allow other urls", url));
      return false;
   }
   return true;
}

bool IsUrlAllowed(const std::string& url) { return SettingAllowExternalUrls() || !IsExternalScheme(url); }

bool IsTableUrl(const std::string& url) { return url.starts_with(kTableOrigin); }

}
