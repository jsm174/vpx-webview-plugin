// license:GPLv3+
#pragma once

#include "include/cef_scheme.h"

namespace WebView {

// Pages are served from vpx://table/, which maps to the folder of the running table and nothing else. Chromium requires
// every process to register the scheme the same way, so the plugin and the helper share this
inline void RegisterTableScheme(CefRawPtr<CefSchemeRegistrar> registrar)
{
   registrar->AddCustomScheme("vpx", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

constexpr const char* kTableOrigin = "vpx://table/";

}
