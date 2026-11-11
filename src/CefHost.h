// license:GPLv3+
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace WebView {

// Starts CEF on first use and (re)enables the message pump. CEF lives until the process exits
bool StartCef();
bool IsCefRunning();
void PauseCef();

// Runs the CEF message loop from the main thread until done() or the timeout
void PumpCefUntil(const std::function<bool()>& done, double timeoutSeconds);

// The folder vpx://table/ is served from
void SetTableRoot(const std::filesystem::path& root);

// Turns a url given by the table script into the url to load. Relative urls map into vpx://table/. Anything else is an
// external url, which needs the AllowExternalUrls setting. Returns false when the url is refused
bool ResolveUrl(const std::string& url, std::string& resolved);

// Whether a page or one of its resources may load this url
bool IsUrlAllowed(const std::string& url);

// Whether this url is served from the table folder, which is what grants table access
bool IsTableUrl(const std::string& url);

}
