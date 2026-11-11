// license:GPLv3+
#pragma once

#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/MsgPlugin.h"
#include "plugins/PluginStrings.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/VPXPlugin.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

using namespace std::string_literals;

namespace WebView {

LPI_USE_CPP();

extern const MsgPluginAPI* msgApi;
extern VPXPluginAPI* vpxApi;
extern ScriptablePluginAPI* scriptApi;
extern uint32_t endpointId;

// Smallest delay that still goes through the main thread queue: RunOnMainThread with a zero delay runs the callback immediately
// when called from the main thread, which would re-enter CEF's message loop from inside it
constexpr double kQueued = 0.0001;

bool SettingDevTools();
bool SettingAllowExternalUrls();

std::filesystem::path PluginDirectory();
void PinModule();
std::string JsonEscape(const std::string& s);
std::string VbsString(const std::string& s);
bool IsIdentifier(const std::string& name);

}

#define LOGD WebView::LPI_LOGD_CPP
#define LOGI WebView::LPI_LOGI_CPP
#define LOGW WebView::LPI_LOGW_CPP
#define LOGE WebView::LPI_LOGE_CPP
