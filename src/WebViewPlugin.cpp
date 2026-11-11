// license:GPLv3+

#include "Audio.h"
#include "CefHost.h"
#include "Controller.h"
#include "Pages.h"
#include "Plugin.h"
#include "TableBridge.h"
#include "WebViewVersion.h"

namespace WebView {

LPI_IMPLEMENT_CPP

const MsgPluginAPI* msgApi = nullptr;
VPXPluginAPI* vpxApi = nullptr;
ScriptablePluginAPI* scriptApi = nullptr;
uint32_t endpointId = 0;

namespace {

unsigned int getVpxApiMsgId = 0;
unsigned int getScriptApiMsgId = 0;
unsigned int onGameEndMsgId = 0;

MSGPI_BOOL_VAL_SETTING(devToolsProp, "DevTools", "Remote debugging", "Expose the pages to Chromium remote debugging on port 9222 (open chrome://inspect)", true, false);
MSGPI_BOOL_VAL_SETTING(allowExternalProp, "AllowExternalUrls", "Allow external urls", "Let pages load from the internet and from files outside of the table folder", true, false);

// Ends everything a game started. CEF itself stays alive for the next game
void Suspend()
{
   CloseAllPages();
   PauseCef();
   ResetBridge();
}

void MSGPIAPI OnGameEnd(const unsigned int, void*, void*) { Suspend(); }

}

bool SettingDevTools() { return devToolsProp_Get() != 0; }
bool SettingAllowExternalUrls() { return allowExternalProp_Get() != 0; }

}

using namespace WebView;

MSGPI_EXPORT void MSGPIAPI WebViewPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   PinModule();
   msgApi = api;
   endpointId = sessionId;
   LPISetup(endpointId, msgApi);

   msgApi->RegisterSetting(endpointId, &devToolsProp);
   msgApi->RegisterSetting(endpointId, &allowExternalProp);

   getVpxApiMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, getVpxApiMsgId, &vpxApi);
   getScriptApiMsgId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, getScriptApiMsgId, &scriptApi);
   msgApi->SubscribeMsg(endpointId, onGameEndMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnGameEnd, nullptr);

   RegisterDisplays();
   RegisterAudio();
   RegisterScriptClasses();

   LOGI("WebView plugin v" WEBVIEW_PLUGIN_VERSION " loaded"s);
}

MSGPI_EXPORT void MSGPIAPI WebViewPluginUnload()
{
   Suspend();
   UnregisterScriptClasses();
   UnregisterAudio();
   UnregisterDisplays();

   msgApi->UnsubscribeMsg(onGameEndMsgId, OnGameEnd, nullptr);
   msgApi->ReleaseMsgID(onGameEndMsgId);
   msgApi->ReleaseMsgID(getVpxApiMsgId);
   msgApi->ReleaseMsgID(getScriptApiMsgId);

   vpxApi = nullptr;
   scriptApi = nullptr;
   msgApi = nullptr;
   endpointId = 0;
}
