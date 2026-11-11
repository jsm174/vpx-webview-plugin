// license:GPLv3+
#pragma once

#include <cstdint>
#include <vector>

namespace WebView {

void RegisterAudio();
void UnregisterAudio();

// Sample rate pages should render at so that VPX does not resample
unsigned int AudioOutputRate();

// Ends the VPX stream of a page
void CloseAudioStream(uint32_t pageId);

// Turns the packets Chromium delivers for one page into a VPX audio stream. Used from Chromium's audio thread
class AudioForwarder final
{
public:
   explicit AudioForwarder(uint32_t pageId)
      : m_pageId(pageId)
   {
   }

   void Start(int sampleRate, int channels);
   void Packet(const float** data, int frames);

private:
   // A sound keeps its inner silences, so that its timing is preserved, and ends after this much continuous silence
   static constexpr int kSilenceTailMs = 250;

   const uint32_t m_pageId;
   int m_sampleRate = 48000;
   int m_channels = 0;
   bool m_playing = false;
   int m_silentFrames = 0;
};

}
