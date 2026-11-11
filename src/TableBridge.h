// license:GPLv3+
#pragma once

#include "include/cef_values.h"

#include <cstdint>
#include <string>

namespace WebView {

// Page -> table script events (vpx.emit and "loaded"), delivered to the Sub named by the table. Any thread
void QueueEvent(const std::string& page, std::string name, std::string data);
void SetEventHandler(const std::string& name);
std::string GetEventHandler();

// vpx.table requests. Pages send names and typed values, never code: the plugin validates the names, converts the values
// to VBScript literals and builds the statement. 'allowed' is whether the requesting frame comes from the table folder
void QueueTableRequest(uint32_t pageId, int request, bool allowed, std::string kind, std::string part, std::string member, CefRefPtr<CefListValue> args);

// Called by the helper script injected into the table to answer a request. The token is renewed for every game, so table
// scripts and pages cannot forge answers
void BridgeAnswer(int token, int pageId, int request, bool ok, const std::string& payload);

void ResetBridge();

}
