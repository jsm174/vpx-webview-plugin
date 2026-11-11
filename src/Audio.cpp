// license:GPLv3+

#include "Audio.h"

#include "Pages.h"
#include "Plugin.h"

#include <cstring>
#include <memory>

namespace WebView {

namespace {

std::unique_ptr<PinballPlugin::Controller::CtrlItemProvider<AudioSrcId>> audioProvider;
unsigned int onAudioUpdateMsgId = 0;
unsigned int getAudioOutputMsgId = 0;

CtlResId AudioSourceId() { return { .endpointId = endpointId, .resId = 0 }; }

CtlResId AudioStreamId(uint32_t pageId) { return { .endpointId = endpointId, .resId = pageId + 1 }; }

void MSGPIAPI PostAudio(void* userData)
{
   auto* msg = static_cast<AudioUpdateMsg*>(userData);
   float volume = 1.f;
   if (msgApi && msg->streamId.endpointId == endpointId && GetPageVolume(msg->streamId.resId - 1, volume))
   {
      msg->volume = volume;
      msgApi->BroadcastMsg(endpointId, onAudioUpdateMsgId, msg);
   }
   delete[] msg->buffer;
   delete msg;
}

}

void RegisterAudio()
{
   onAudioUpdateMsgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_ON_UPDATE_MSG);
   getAudioOutputMsgId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_GET_OUTPUT_MSG);
   audioProvider = std::make_unique<PinballPlugin::Controller::CtrlItemProvider<AudioSrcId>>(msgApi, endpointId, CTLPI_AUDIO_GET_SRC_MSG, CTLPI_AUDIO_ON_SRC_CHG_MSG);
   audioProvider->SetItem({ .id = AudioSourceId(), .overrideId = {}, .name = "WebView", .desc = "Audio played by WebView pages", .target = CTLPI_AUDIO_TARGET_BACKGLASS });
}

void UnregisterAudio()
{
   audioProvider.reset();
   msgApi->ReleaseMsgID(onAudioUpdateMsgId);
   msgApi->ReleaseMsgID(getAudioOutputMsgId);
}

unsigned int AudioOutputRate()
{
   GetAudioOutputMsg msg { CTLPI_AUDIO_TARGET_BACKGLASS, 0 };
   msgApi->BroadcastMsg(endpointId, getAudioOutputMsgId, &msg);
   return msg.sampleRate ? msg.sampleRate : 48000;
}

void CloseAudioStream(uint32_t pageId)
{
   if (!msgApi)
      return;
   AudioUpdateMsg msg {};
   msg.sourceId = AudioSourceId();
   msg.streamId = AudioStreamId(pageId);
   msg.buffer = nullptr;
   msgApi->BroadcastMsg(endpointId, onAudioUpdateMsgId, &msg);
}

void AudioForwarder::Start(int sampleRate, int channels)
{
   m_sampleRate = sampleRate;
   m_channels = channels;
   m_playing = false;
   m_silentFrames = 0;
}

void AudioForwarder::Packet(const float** data, int frames)
{
   if (frames <= 0 || m_channels <= 0 || !msgApi)
      return;
   const float* left = data[0];
   const float* right = m_channels > 1 ? data[1] : data[0];
   bool silent = true;
   for (int i = 0; i < frames && silent; i++)
      silent = left[i] == 0.f && right[i] == 0.f;
   if (silent)
   {
      if (!m_playing)
         return;
      m_silentFrames += frames;
      if (m_silentFrames >= m_sampleRate * kSilenceTailMs / 1000)
      {
         m_playing = false;
         return;
      }
   }
   else
   {
      m_playing = true;
      m_silentFrames = 0;
   }

   const size_t size = static_cast<size_t>(frames) * 2 * sizeof(float);
   auto* msg = new AudioUpdateMsg {};
   msg->sourceId = AudioSourceId();
   msg->streamId = AudioStreamId(m_pageId);
   msg->channelFormat = CTLPI_AUDIO_FORMAT_CHANNEL_STEREO;
   msg->sampleFormat = CTLPI_AUDIO_FORMAT_SAMPLE_FLOAT;
   msg->sampleRate = m_sampleRate;
   msg->volume = 1.f;
   msg->bufferSize = static_cast<unsigned int>(size);
   msg->buffer = new uint8_t[size];
   auto* out = reinterpret_cast<float*>(msg->buffer);
   for (int i = 0; i < frames; i++)
   {
      out[2 * i] = left[i];
      out[2 * i + 1] = right[i];
   }
   msgApi->RunOnMainThread(endpointId, kQueued, PostAudio, msg);
}

}
