#pragma once

#include <atomic>


#include <modules/audio_device/include/audio_device.h>
#include <api/scoped_refptr.h>

#include "Utils.h"

namespace hope {
    namespace rtc {

        class AudioDeviceModuleImpl : public webrtc::AudioDeviceModule {
        public:
            AudioDeviceModuleImpl();
            ~AudioDeviceModuleImpl();

            static webrtc::scoped_refptr<AudioDeviceModuleImpl> Create();

            // Key method: Push audio data into WebRTC
            void PushAudioData(unsigned char* data, size_t size);

            // Alternative method for pushing raw PCM frames directly
            void PushPcmFrame(const int16_t* samples, size_t numSamples, int sampleRate, int channels);

            // Register audio callback - this is called by WebRTC
            int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override;

            // RefCounted interface
            void AddRef() const override;
            webrtc::RefCountReleaseStatus Release() const override;

            // Initialization methods
            int32_t Init() override;
            int32_t Terminate() override;
            bool Initialized() const override;

            // Device enumeration (stub implementations for virtual device)
            int16_t PlayoutDevices() override;
            int16_t RecordingDevices() override;

            int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                char guid[webrtc::kAdmMaxGuidSize]) override;

            int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                char guid[webrtc::kAdmMaxGuidSize]) override;

            // Device selection
            int32_t SetPlayoutDevice(uint16_t index) override;
            int32_t SetPlayoutDevice(WindowsDeviceType device) override;
            int32_t SetRecordingDevice(uint16_t index) override;
            int32_t SetRecordingDevice(WindowsDeviceType device) override;

            // Playout methods
            int32_t PlayoutIsAvailable(bool* available) override;
            int32_t InitPlayout() override;
            bool PlayoutIsInitialized() const override;
            int32_t StartPlayout() override;
            int32_t StopPlayout() override;
            bool Playing() const override;

            // Recording methods
            int32_t RecordingIsAvailable(bool* available) override;
            int32_t InitRecording() override;
            bool RecordingIsInitialized() const override;
            int32_t StartRecording() override;
            int32_t StopRecording() override;
            bool Recording() const override;

            // Speaker/Microphone initialization
            int32_t InitSpeaker() override;
            bool SpeakerIsInitialized() const override;
            int32_t InitMicrophone() override;
            bool MicrophoneIsInitialized() const override;

            // Volume controls (stub implementations)
            int32_t SpeakerVolumeIsAvailable(bool* available) override;
            int32_t SetSpeakerVolume(uint32_t volume) override;
            int32_t SpeakerVolume(uint32_t* volume) const override;
            int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override;
            int32_t MinSpeakerVolume(uint32_t* minVolume) const override;

            int32_t MicrophoneVolumeIsAvailable(bool* available) override;
            int32_t SetMicrophoneVolume(uint32_t volume) override;
            int32_t MicrophoneVolume(uint32_t* volume) const override;
            int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const override;
            int32_t MinMicrophoneVolume(uint32_t* minVolume) const override;

            // Mute controls
            int32_t SpeakerMuteIsAvailable(bool* available) override;
            int32_t SetSpeakerMute(bool enable) override;
            int32_t SpeakerMute(bool* enabled) const override;

            int32_t MicrophoneMuteIsAvailable(bool* available) override;
            int32_t SetMicrophoneMute(bool enable) override;
            int32_t MicrophoneMute(bool* enabled) const override;

            // Stereo support
            int32_t StereoPlayoutIsAvailable(bool* available) const override;
            int32_t SetStereoPlayout(bool enable) override;
            int32_t StereoPlayout(bool* enabled) const override;

            int32_t StereoRecordingIsAvailable(bool* available) const override;
            int32_t SetStereoRecording(bool enable) override;
            int32_t StereoRecording(bool* enabled) const override;

            // Delay
            int32_t PlayoutDelay(uint16_t* delayMS) const override;

            // Built-in audio processing
            bool BuiltInAECIsAvailable() const override;
            bool BuiltInAGCIsAvailable() const override;
            bool BuiltInNSIsAvailable() const override;

            int32_t EnableBuiltInAEC(bool enable) override;
            int32_t EnableBuiltInAGC(bool enable) override;
            int32_t EnableBuiltInNS(bool enable) override;

            // Active audio layer
            int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override;

        private:
            webrtc::AudioTransport* audioHandle;
            std::atomic<bool> recording;
            std::atomic<bool> playing;
            std::atomic<bool> initialized;
            std::atomic<bool> speakerInitialized;
            std::atomic<bool> microphoneInitialized;
            std::atomic<bool> recordingInitialized;
            std::atomic<bool> playoutInitialized;
            std::atomic<bool> stereoRecording;

            // Audio buffer for accumulating data
            std::vector<uint8_t> audioBuffer;
        };

    } // namespace rtc
} // namespace hope
