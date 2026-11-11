// license:GPLv3+

#include "Pages.h"

#include "Audio.h"
#include "CefHost.h"
#include "Plugin.h"
#include "TableBridge.h"

#include "include/cef_browser.h"
#include "include/cef_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace WebView {

namespace {

class PageClient;

// One painted page frame, premultiplied BGRA
struct Frame
{
   std::vector<uint8_t> pixels;
   int width = 0;
   int height = 0;
};

struct Placement
{
   uint32_t screen;
   int x;
   int y;
   int width;
   int height;
};

// Web content. A page has no output of its own: it renders at the size of its largest placement and its latest frame is
// composited into every screen it is placed on. A page that is not placed still runs (logic, audio, events).
struct Page
{
   uint32_t id = 0;
   std::string name;
   CefRefPtr<PageClient> client;
   float volume = 1.f;
   int layer = 0;
   std::vector<Placement> placements;
   // Frames are immutable once published, so the compositor can read them without holding the state lock. Painting reuses
   // the pool buffers nobody else references any more
   std::shared_ptr<Frame> frame;
   std::array<std::shared_ptr<Frame>, 3> pool;
};

// A virtual screen: a display source for flashers (ctrl://webview/display?id=N) that can also be shown on ancillary
// windows. The compositor thread blends the screen's pages into 'canvas', then publishes it triple buffered: it fills
// 'write' and swaps it with 'pending', consumers swap 'pending' with 'front' and read 'front'.
struct Screen
{
   Screen(uint32_t screenId, uint32_t w, uint32_t h)
      : id(screenId)
      , width(w)
      , height(h)
      , canvas(static_cast<size_t>(w) * h * 4, 0)
   {
      for (auto& slot : slots)
      {
         slot.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
         slot.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
      }
   }

   struct Slot
   {
      std::vector<uint8_t> rgb;
      std::vector<uint8_t> rgba;
      unsigned int frameId = 0;
   };

   const uint32_t id;
   const uint32_t width;
   const uint32_t height;
   std::atomic<int> windows { 0 };
   bool dirty = true;
   std::vector<uint8_t> canvas;

   std::mutex mutex;
   std::array<Slot, 3> slots;
   int write = 0;
   int pending = 1;
   int front = 2;
   bool hasPending = false;
   unsigned int frameCounter = 0;

   VPXTexture texture = nullptr;
   unsigned int textureFrameId = 0;

   void Publish()
   {
      Slot& slot = slots[write];
      const uint8_t* src = canvas.data();
      uint8_t* rgb = slot.rgb.data();
      const size_t count = static_cast<size_t>(width) * height;
      for (size_t i = 0; i < count; i++, src += 4, rgb += 3)
      {
         rgb[0] = src[2];
         rgb[1] = src[1];
         rgb[2] = src[0];
      }
      if (windows != 0)
      {
         src = canvas.data();
         uint8_t* rgba = slot.rgba.data();
         for (size_t i = 0; i < count; i++, src += 4, rgba += 4)
         {
            const unsigned int a = src[3];
            if (a == 255u || a == 0u)
            {
               rgba[0] = src[2];
               rgba[1] = src[1];
               rgba[2] = src[0];
            }
            else
            {
               rgba[0] = static_cast<uint8_t>(std::min(255u, (src[2] * 255u + a / 2) / a));
               rgba[1] = static_cast<uint8_t>(std::min(255u, (src[1] * 255u + a / 2) / a));
               rgba[2] = static_cast<uint8_t>(std::min(255u, (src[0] * 255u + a / 2) / a));
            }
            rgba[3] = static_cast<uint8_t>(a);
         }
      }
      std::lock_guard lock(mutex);
      slot.frameId = ++frameCounter;
      std::swap(write, pending);
      hasPending = true;
   }

   const Slot& Latest()
   {
      std::lock_guard lock(mutex);
      if (hasPending)
      {
         std::swap(pending, front);
         hasPending = false;
      }
      return slots[front];
   }
};

std::unique_ptr<PinballPlugin::Controller::CtrlItemProvider<DisplaySrcId>> displayProvider;
unsigned int getAuxRendererMsgId = 0;
unsigned int onAuxRendererChgMsgId = 0;

// Guards pages, screens and the compositor flags
std::mutex stateMutex;
std::map<std::string, std::unique_ptr<Page>> pages;
std::map<uint32_t, std::shared_ptr<Screen>> screens;
uint32_t nextPageId = 0;
std::atomic<unsigned int> audioRate { 48000 };
std::atomic<int> openBrowsers { 0 };

std::thread compositorThread;
std::condition_variable compositorWake;
bool compositorStop = false;
bool composePending = false;

Page* FindPage(uint32_t id)
{
   for (auto& [name, page] : pages)
      if (page->id == id)
         return page.get();
   return nullptr;
}

void MarkScreensDirty(const Page& page)
{
   for (const auto& placement : page.placements)
      if (auto it = screens.find(placement.screen); it != screens.end())
         it->second->dirty = true;
   composePending = true;
}

int WindowMask(const std::string& names)
{
   int mask = 0;
   std::string name;
   for (size_t i = 0; i <= names.size(); i++)
   {
      if (i < names.size() && names[i] != ',' && names[i] != ' ')
      {
         name += static_cast<char>(std::tolower(static_cast<unsigned char>(names[i])));
         continue;
      }
      if (name == "backglass")
         mask |= 1 << VPXWINDOW_Backglass;
      else if (name == "scoreview" || name == "dmd")
         mask |= 1 << VPXWINDOW_ScoreView;
      else if (name == "topper")
         mask |= 1 << VPXWINDOW_Topper;
      else if (!name.empty())
         LOGW(std::format("Unknown window '{}', expected backglass, scoreview or topper", name));
      name.clear();
   }
   return mask;
}

std::filesystem::path TableDirectory()
{
   VPXTableInfo info {};
   vpxApi->GetTableInfo(&info);
   return info.path ? PluginStrings::PathFromNative(info.path).parent_path() : std::filesystem::path();
}

///////////////////////////////////////////////////////////////////////////////
// Compositing

inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t t)
{
   const uint32_t rb = (((a & 0x00FF00FFu) * (256u - t) + (b & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
   const uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * (256u - t) + ((b >> 8) & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
   return rb | (ag << 8);
}

struct Tap
{
   int first;
   int second;
   uint32_t weight;
};

std::vector<Tap> Taps(int from, int to, int origin, int size, int sourceSize)
{
   std::vector<Tap> taps;
   taps.reserve(to - from);
   for (int d = from; d < to; d++)
   {
      const float f = std::clamp((static_cast<float>(d - origin) + 0.5f) * static_cast<float>(sourceSize) / static_cast<float>(size) - 0.5f, 0.f, static_cast<float>(sourceSize - 1));
      const int first = static_cast<int>(f);
      taps.push_back({ first, std::min(first + 1, sourceSize - 1), static_cast<uint32_t>((f - static_cast<float>(first)) * 256.f) });
   }
   return taps;
}

// Draws a page frame into a screen canvas with "over" blending. Frames at another size than their placement are sampled
// bilinearly. Opaque and fully transparent pixels, which are most of them, skip the blend.
void Blend(Screen& screen, const Frame& frame, const Placement& placement)
{
   if (frame.pixels.empty() || placement.width <= 0 || placement.height <= 0)
      return;
   const int x0 = std::max(placement.x, 0);
   const int y0 = std::max(placement.y, 0);
   const int x1 = std::min(placement.x + placement.width, static_cast<int>(screen.width));
   const int y1 = std::min(placement.y + placement.height, static_cast<int>(screen.height));
   if (x0 >= x1 || y0 >= y1)
      return;
   const bool scaled = frame.width != placement.width || frame.height != placement.height;
   const std::vector<Tap> columns = scaled ? Taps(x0, x1, placement.x, placement.width, frame.width) : std::vector<Tap>();
   const std::vector<Tap> rows = scaled ? Taps(y0, y1, placement.y, placement.height, frame.height) : std::vector<Tap>();
   const auto* source = reinterpret_cast<const uint32_t*>(frame.pixels.data());
   for (int y = y0; y < y1; y++)
   {
      const uint32_t* top = source + static_cast<size_t>(scaled ? rows[y - y0].first : y - placement.y) * frame.width;
      const uint32_t* bottom = scaled ? source + static_cast<size_t>(rows[y - y0].second) * frame.width : top;
      const uint32_t rowWeight = scaled ? rows[y - y0].weight : 0;
      auto* dst = reinterpret_cast<uint32_t*>(screen.canvas.data()) + static_cast<size_t>(y) * screen.width + x0;
      for (int i = 0; i < x1 - x0; i++)
      {
         uint32_t src;
         if (scaled)
         {
            const Tap& c = columns[i];
            src = Lerp(Lerp(top[c.first], top[c.second], c.weight), Lerp(bottom[c.first], bottom[c.second], c.weight), rowWeight);
         }
         else
            src = top[x0 - placement.x + i];
         const uint32_t a = src >> 24;
         if (a == 255u)
            dst[i] = src;
         else if (a != 0u)
         {
            const uint32_t inv = 255u - a;
            const uint32_t d = dst[i];
            const uint32_t rb = (((d & 0x00FF00FFu) * inv + 0x00800080u) >> 8) & 0x00FF00FFu;
            const uint32_t ag = ((((d >> 8) & 0x00FF00FFu) * inv + 0x00800080u) >> 8) & 0x00FF00FFu;
            dst[i] = src + (rb | (ag << 8));
         }
      }
   }
}

// Composites off the main thread: it only takes the state lock to collect what changed
void CompositorMain()
{
   struct Layer
   {
      std::shared_ptr<const Frame> frame;
      Placement placement;
      int layer;
   };
   struct Job
   {
      std::shared_ptr<Screen> screen;
      std::vector<Layer> layers;
   };

   std::unique_lock lock(stateMutex);
   while (true)
   {
      compositorWake.wait(lock, [] { return compositorStop || composePending; });
      if (compositorStop)
         return;
      composePending = false;
      std::vector<Job> jobs;
      for (auto& [screenId, screen] : screens)
      {
         if (!screen->dirty)
            continue;
         screen->dirty = false;
         Job job { screen, {} };
         for (const auto& [name, page] : pages)
            if (page->frame)
               for (const auto& placement : page->placements)
                  if (placement.screen == screenId)
                     job.layers.push_back({ page->frame, placement, page->layer });
         std::ranges::stable_sort(job.layers, [](const Layer& a, const Layer& b) { return a.layer < b.layer; });
         jobs.push_back(std::move(job));
      }
      lock.unlock();
      for (auto& job : jobs)
      {
         std::ranges::fill(job.screen->canvas, 0);
         for (const auto& layer : job.layers)
            Blend(*job.screen, *layer.frame, layer.placement);
         job.screen->Publish();
      }
      jobs.clear();
      lock.lock();
   }
}

void StartCompositor()
{
   if (compositorThread.joinable())
      return;
   {
      std::lock_guard lock(stateMutex);
      compositorStop = false;
   }
   compositorThread = std::thread(CompositorMain);
}

void StopCompositor()
{
   if (!compositorThread.joinable())
      return;
   {
      std::lock_guard lock(stateMutex);
      compositorStop = true;
   }
   compositorWake.notify_one();
   compositorThread.join();
}

void StorePageFrame(uint32_t pageId, int width, int height, const uint8_t* bgra)
{
   std::shared_ptr<Frame> target;
   {
      std::lock_guard lock(stateMutex);
      Page* page = FindPage(pageId);
      if (page == nullptr || page->placements.empty())
         return;
      for (auto& buffer : page->pool)
      {
         if (!buffer)
            buffer = std::make_shared<Frame>();
         if (buffer.use_count() == 1)
         {
            target = buffer;
            break;
         }
      }
      if (!target)
         target = std::make_shared<Frame>();
   }
   target->pixels.assign(bgra, bgra + static_cast<size_t>(width) * height * 4);
   target->width = width;
   target->height = height;
   {
      std::lock_guard lock(stateMutex);
      Page* page = FindPage(pageId);
      if (page == nullptr)
         return;
      page->frame = target;
      MarkScreensDirty(*page);
   }
   compositorWake.notify_one();
}

///////////////////////////////////////////////////////////////////////////////
// Browser client

class PageClient final
   : public CefClient
   , public CefRenderHandler
   , public CefAudioHandler
   , public CefLoadHandler
   , public CefDisplayHandler
   , public CefLifeSpanHandler
   , public CefRequestHandler
   , public CefResourceRequestHandler
{
public:
   PageClient(uint32_t id, std::string name)
      : m_id(id)
      , m_name(std::move(name))
      , m_audio(id)
   {
   }

   CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
   CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
   CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
   CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
   CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
   CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

   void Run(std::function<void(CefRefPtr<CefBrowser>)> action)
   {
      if (m_browser)
         action(m_browser);
      else if (!m_closing)
         m_pending.push_back(std::move(action));
   }

   void Resize(int width, int height)
   {
      if (width == m_width && height == m_height)
         return;
      m_width = width;
      m_height = height;
      if (m_browser)
         m_browser->GetHost()->WasResized();
   }

   void Close()
   {
      m_closing = true;
      m_pending.clear();
      if (m_browser)
         m_browser->GetHost()->CloseBrowser(true);
   }

   // CefLifeSpanHandler
   void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
   {
      m_browser = browser;
      openBrowsers++;
      // CEF keeps playing a page's audio to the system output even while the audio handler captures it
      browser->GetHost()->SetAudioMuted(true);
      if (m_closing)
      {
         browser->GetHost()->CloseBrowser(true);
         return;
      }
      for (auto& action : m_pending)
         action(browser);
      m_pending.clear();
   }

   void OnBeforeClose(CefRefPtr<CefBrowser>) override
   {
      m_browser = nullptr;
      openBrowsers--;
   }

   // CefRenderHandler
   void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, std::max(m_width.load(), 1), std::max(m_height.load(), 1)); }

   bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override
   {
      info.device_scale_factor = 1.f;
      info.rect = CefRect(0, 0, std::max(m_width.load(), 1), std::max(m_height.load(), 1));
      info.available_rect = info.rect;
      return true;
   }

   void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const void* buffer, int width, int height) override
   {
      if (type == PET_VIEW && width > 0 && height > 0 && m_width > 0)
         StorePageFrame(m_id, width, height, static_cast<const uint8_t*>(buffer));
   }

   // CefAudioHandler
   bool GetAudioParameters(CefRefPtr<CefBrowser>, CefAudioParameters& params) override
   {
      params.sample_rate = static_cast<int>(audioRate.load());
      params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
      params.frames_per_buffer = static_cast<int>(audioRate.load() / 100);
      return true;
   }

   void OnAudioStreamStarted(CefRefPtr<CefBrowser>, const CefAudioParameters& params, int channels) override { m_audio.Start(params.sample_rate, channels); }

   void OnAudioStreamPacket(CefRefPtr<CefBrowser>, const float** data, int frames, int64_t) override { m_audio.Packet(data, frames); }

   void OnAudioStreamStopped(CefRefPtr<CefBrowser>) override { }

   void OnAudioStreamError(CefRefPtr<CefBrowser>, const CefString& message) override { LOGW(std::format("[{}] audio stream error: {}", m_name, message.ToString())); }

   // CefLoadHandler
   void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
   {
      if (frame->IsMain())
         QueueEvent(m_name, "loaded", frame->GetURL().ToString());
   }

   void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) override
   {
      if (errorCode != ERR_ABORTED)
         LOGE(std::format("[{}] failed to load {}: {} ({})", m_name, failedUrl.ToString(), errorText.ToString(), static_cast<int>(errorCode)));
   }

   // CefDisplayHandler
   bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message, const CefString&, int) override
   {
      const std::string text = std::format("[{}] {}", m_name, message.ToString());
      if (level >= LOGSEVERITY_ERROR)
         LOGE(text);
      else if (level == LOGSEVERITY_WARNING)
         LOGW(text);
      else
         LOGI(text);
      return true;
   }

   // CefRequestHandler: pages stay inside the table folder unless external urls are allowed
   bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, bool, bool) override
   {
      const std::string url = request->GetURL().ToString();
      if (IsUrlAllowed(url))
         return false;
      LOGW(std::format("[{}] navigation to {} blocked, set AllowExternalUrls = 1 to allow it", m_name, url));
      return true;
   }

   CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) override
   {
      return this;
   }

   // CefResourceRequestHandler
   ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback>) override
   {
      const std::string url = request->GetURL().ToString();
      if (IsUrlAllowed(url))
         return RV_CONTINUE;
      LOGW(std::format("[{}] request to {} blocked, set AllowExternalUrls = 1 to allow it", m_name, url));
      return RV_CANCEL;
   }

   // CefClient
   bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId, CefRefPtr<CefProcessMessage> message) override
   {
      const auto args = message->GetArgumentList();
      if (message->GetName() == "vpx.emit")
      {
         QueueEvent(m_name, args->GetString(0).ToString(), args->GetString(1).ToString());
         return true;
      }
      if (message->GetName() == "vpx.table")
      {
         QueueTableRequest(m_id, args->GetInt(0), IsTableUrl(frame->GetURL().ToString()), args->GetString(1).ToString(), args->GetString(2).ToString(),
            args->GetString(3).ToString(), args->GetList(4)->Copy());
         return true;
      }
      return false;
   }

private:
   const uint32_t m_id;
   const std::string m_name;
   std::atomic<int> m_width { 0 };
   std::atomic<int> m_height { 0 };
   CefRefPtr<CefBrowser> m_browser;
   std::vector<std::function<void(CefRefPtr<CefBrowser>)>> m_pending;
   bool m_closing = false;
   AudioForwarder m_audio;

   IMPLEMENT_REFCOUNTING(PageClient);
};

///////////////////////////////////////////////////////////////////////////////
// VPX outputs

DisplayFrame MSGPIAPI GetRenderFrame(void* context)
{
   const Screen::Slot& slot = static_cast<Screen*>(context)->Latest();
   return { slot.frameId, slot.rgb.data() };
}

void AdvertiseDisplays()
{
   if (!displayProvider)
      return;
   displayProvider->ClearItems();
   std::vector<DisplaySrcId> items;
   {
      std::lock_guard lock(stateMutex);
      for (auto& [id, screen] : screens)
      {
         DisplaySrcId src {};
         src.id = { .endpointId = endpointId, .resId = id };
         src.width = screen->width;
         src.height = screen->height;
         src.hardware = CTLPI_DISPLAY_HARDWARE_LCD_DISPLAY;
         src.callContext = screen.get();
         src.frameFormat = CTLPI_DISPLAY_FORMAT_SRGB888;
         src.GetRenderFrame = GetRenderFrame;
         items.push_back(src);
      }
   }
   if (!items.empty())
      displayProvider->AddItems(items);
}

int MSGPIAPI RenderWindow(VPXRenderContext2D* ctx, void*)
{
   std::lock_guard lock(stateMutex);
   for (auto& [id, screen] : screens)
   {
      if ((screen->windows & (1 << ctx->window)) == 0)
         continue;
      const Screen::Slot& slot = screen->Latest();
      if (slot.frameId == 0)
         return 0;
      if (screen->texture == nullptr || screen->textureFrameId != slot.frameId)
      {
         vpxApi->UpdateTexture(&screen->texture, static_cast<int>(screen->width), static_cast<int>(screen->height), VPXTEXFMT_sRGBA8, slot.rgba.data());
         screen->textureFrameId = slot.frameId;
      }
      const auto w = static_cast<float>(screen->width);
      const auto h = static_cast<float>(screen->height);
      ctx->srcWidth = w;
      ctx->srcHeight = h;
      ctx->DrawImage(ctx, screen->texture, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, w, h, 0.f, 0.f, 0.f, 0.f, 0.f, w, h);
      return 1;
   }
   return 0;
}

void MSGPIAPI OnGetAuxRenderer(const unsigned int, void*, void* msgData)
{
   static constexpr AncillaryRendererDef entry = { "WebView", "WebView", "Web pages rendered by the WebView plugin", nullptr, RenderWindow };
   auto* msg = static_cast<GetAncillaryRendererMsg*>(msgData);
   if (msg->window == VPXWINDOW_Backglass || msg->window == VPXWINDOW_ScoreView || msg->window == VPXWINDOW_Topper)
   {
      if (msg->count < msg->maxEntryCount)
         msg->entries[msg->count] = entry;
      msg->count++;
   }
}

void ReleasePage(std::unique_ptr<Page> page)
{
   CloseAudioStream(page->id);
   page->client->Close();
}

void ReleaseScreen(const std::shared_ptr<Screen>& screen)
{
   if (screen->texture && vpxApi)
      vpxApi->DeleteTexture(screen->texture);
   screen->texture = nullptr;
}

CefRefPtr<PageClient> FindClient(const std::string& name)
{
   std::lock_guard lock(stateMutex);
   const auto it = pages.find(name);
   return it == pages.end() ? nullptr : it->second->client;
}

}

void RegisterDisplays()
{
   displayProvider = std::make_unique<PinballPlugin::Controller::CtrlItemProvider<DisplaySrcId>>(msgApi, endpointId, CTLPI_DISPLAY_GET_SRC_MSG, CTLPI_DISPLAY_ON_SRC_CHG_MSG);
   msgApi->SubscribeMsg(endpointId, getAuxRendererMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_AUX_RENDERER), OnGetAuxRenderer, nullptr);
   msgApi->BroadcastMsg(endpointId, onAuxRendererChgMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_AUX_RENDERER_CHG), nullptr);
}

void UnregisterDisplays()
{
   displayProvider.reset();
   msgApi->UnsubscribeMsg(getAuxRendererMsgId, OnGetAuxRenderer, nullptr);
   msgApi->BroadcastMsg(endpointId, onAuxRendererChgMsgId, nullptr);
   msgApi->ReleaseMsgID(getAuxRendererMsgId);
   msgApi->ReleaseMsgID(onAuxRendererChgMsgId);
}

void CreateScreen(int id, int width, int height)
{
   if (id < 0 || width <= 0 || height <= 0 || width > 8192 || height > 8192)
   {
      LOGE(std::format("Invalid screen {} size {}x{}", id, width, height));
      return;
   }
   const auto uid = static_cast<uint32_t>(id);
   std::shared_ptr<Screen> replaced;
   {
      std::lock_guard lock(stateMutex);
      const auto it = screens.find(uid);
      if (it != screens.end() && it->second->width == static_cast<uint32_t>(width) && it->second->height == static_cast<uint32_t>(height))
         return;
      auto screen = std::make_shared<Screen>(uid, width, height);
      if (it != screens.end())
      {
         screen->windows = it->second->windows.load();
         replaced = it->second;
      }
      screens[uid] = std::move(screen);
      composePending = true;
   }
   AdvertiseDisplays();
   if (replaced)
      ReleaseScreen(replaced);
   StartCompositor();
   compositorWake.notify_one();
}

void CloseScreen(int id)
{
   std::shared_ptr<Screen> screen;
   {
      std::lock_guard lock(stateMutex);
      const auto it = screens.find(static_cast<uint32_t>(id));
      if (it == screens.end())
         return;
      screen = it->second;
      screens.erase(it);
   }
   AdvertiseDisplays();
   ReleaseScreen(screen);
}

void ShowScreen(int id, const std::string& windows)
{
   {
      std::lock_guard lock(stateMutex);
      if (!screens.contains(static_cast<uint32_t>(id)))
      {
         LOGW(std::format("ShowScreen: screen {} does not exist", id));
         return;
      }
      const int mask = WindowMask(windows);
      for (auto& [sid, screen] : screens)
      {
         if (sid == static_cast<uint32_t>(id))
         {
            screen->windows = mask;
            screen->dirty = true;
         }
         else
            screen->windows = screen->windows & ~mask;
      }
      composePending = true;
   }
   compositorWake.notify_one();
}

void OpenPage(const std::string& name, const std::string& url)
{
   if (name.empty())
   {
      LOGE("Open: a page needs a name"s);
      return;
   }
   if (!StartCef())
      return;
   SetTableRoot(TableDirectory());
   audioRate = AudioOutputRate();
   std::string target;
   if (!ResolveUrl(url, target))
      return;
   LOGI(std::format("[{}] {}", name, target));

   if (CefRefPtr<PageClient> existing = FindClient(name))
   {
      existing->Run([target](CefRefPtr<CefBrowser> browser) { browser->GetMainFrame()->LoadURL(target); });
      return;
   }

   CefRefPtr<PageClient> client;
   {
      std::lock_guard lock(stateMutex);
      auto page = std::make_unique<Page>();
      page->id = nextPageId++;
      page->name = name;
      page->client = client = new PageClient(page->id, name);
      pages[name] = std::move(page);
   }
   CefWindowInfo windowInfo;
   windowInfo.SetAsWindowless(kNullWindowHandle);
   windowInfo.shared_texture_enabled = false;
   CefBrowserSettings browserSettings;
   browserSettings.windowless_frame_rate = 60;
   browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);
   CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
   extra->SetString("name", name);
   CefBrowserHost::CreateBrowser(windowInfo, client, target, browserSettings, extra, nullptr);
}

void ClosePage(const std::string& name)
{
   std::unique_ptr<Page> page;
   {
      std::lock_guard lock(stateMutex);
      const auto it = pages.find(name);
      if (it == pages.end())
         return;
      page = std::move(it->second);
      pages.erase(it);
      MarkScreensDirty(*page);
   }
   ReleasePage(std::move(page));
   compositorWake.notify_one();
}

void PlacePage(const std::string& name, int screenId, int x, int y, int width, int height)
{
   CefRefPtr<PageClient> client;
   int renderWidth = 0;
   int renderHeight = 0;
   {
      std::lock_guard lock(stateMutex);
      const auto it = pages.find(name);
      if (it == pages.end())
      {
         LOGW(std::format("Place: page '{}' is not open", name));
         return;
      }
      if (width > 0 && height > 0 && !screens.contains(static_cast<uint32_t>(screenId)))
         LOGW(std::format("Place: screen {} does not exist yet", screenId));
      Page& page = *it->second;
      MarkScreensDirty(page);
      std::erase_if(page.placements, [screenId](const Placement& p) { return p.screen == static_cast<uint32_t>(screenId); });
      if (width > 0 && height > 0)
         page.placements.push_back({ static_cast<uint32_t>(screenId), x, y, std::min(width, 8192), std::min(height, 8192) });
      MarkScreensDirty(page);
      for (const auto& placement : page.placements)
      {
         if (static_cast<int64_t>(placement.width) * placement.height > static_cast<int64_t>(renderWidth) * renderHeight)
         {
            renderWidth = placement.width;
            renderHeight = placement.height;
         }
      }
      if (page.placements.empty())
         page.frame = nullptr;
      client = page.client;
   }
   client->Resize(renderWidth, renderHeight);
   compositorWake.notify_one();
}

void SetPageLayer(const std::string& name, int layer)
{
   {
      std::lock_guard lock(stateMutex);
      const auto it = pages.find(name);
      if (it == pages.end())
         return;
      it->second->layer = layer;
      MarkScreensDirty(*it->second);
   }
   compositorWake.notify_one();
}

void SetPageVolume(const std::string& name, float volume)
{
   std::lock_guard lock(stateMutex);
   if (const auto it = pages.find(name); it != pages.end())
      it->second->volume = std::clamp(volume, 0.f, 4.f);
}

void SetPageFrameRate(const std::string& name, int fps)
{
   if (CefRefPtr<PageClient> client = FindClient(name))
      client->Run([fps](CefRefPtr<CefBrowser> browser) { browser->GetHost()->SetWindowlessFrameRate(std::clamp(fps, 1, 240)); });
}

void EvalInPage(const std::string& name, const std::string& js)
{
   if (CefRefPtr<PageClient> client = FindClient(name))
      client->Run([js](CefRefPtr<CefBrowser> browser) { browser->GetMainFrame()->ExecuteJavaScript(js, "webview-eval", 1); });
   else
      LOGW(std::format("Page '{}' is not open", name));
}

void EvalInPage(uint32_t pageId, const std::string& js)
{
   CefRefPtr<PageClient> client;
   {
      std::lock_guard lock(stateMutex);
      if (const Page* page = FindPage(pageId))
         client = page->client;
   }
   if (client)
      client->Run([js](CefRefPtr<CefBrowser> browser) { browser->GetMainFrame()->ExecuteJavaScript(js, "webview-eval", 1); });
}

bool GetPageVolume(uint32_t pageId, float& volume)
{
   std::lock_guard lock(stateMutex);
   const Page* page = FindPage(pageId);
   if (page == nullptr)
      return false;
   volume = page->volume;
   return true;
}

void CloseAllPages()
{
   StopCompositor();
   std::vector<std::unique_ptr<Page>> closingPages;
   std::vector<std::shared_ptr<Screen>> closingScreens;
   {
      std::lock_guard lock(stateMutex);
      for (auto& [name, page] : pages)
         closingPages.push_back(std::move(page));
      for (auto& [id, screen] : screens)
         closingScreens.push_back(screen);
      pages.clear();
      screens.clear();
      composePending = false;
   }
   if (displayProvider)
      displayProvider->ClearItems();
   for (auto& page : closingPages)
      ReleasePage(std::move(page));
   for (auto& screen : closingScreens)
      ReleaseScreen(screen);
   PumpCefUntil([] { return openBrowsers <= 0; }, 2.);
}

}
