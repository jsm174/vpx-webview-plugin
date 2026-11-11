// license:GPLv3+
#pragma once

#include <cstdint>
#include <string>

namespace WebView {

// Screens are what VPX sees: a display source per screen (flashers) plus the ancillary windows it is shown on.
// Pages are web content, placed on screens. Everything here is called from the main thread unless noted.

void RegisterDisplays();
void UnregisterDisplays();

void CreateScreen(int id, int width, int height);
void CloseScreen(int id);
void ShowScreen(int id, const std::string& windows);

void OpenPage(const std::string& name, const std::string& url);
void ClosePage(const std::string& name);
void PlacePage(const std::string& name, int screen, int x, int y, int width, int height);
void SetPageLayer(const std::string& name, int layer);
void SetPageVolume(const std::string& name, float volume);
void SetPageFrameRate(const std::string& name, int fps);
void EvalInPage(const std::string& name, const std::string& js);
void EvalInPage(uint32_t pageId, const std::string& js);

// Any thread. Returns false when the page is gone
bool GetPageVolume(uint32_t pageId, float& volume);

void CloseAllPages();

}
