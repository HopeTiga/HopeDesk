#include "AudioDeviceModuleImpl.h"
#include <rtc_base/ref_counted_object.h>

namespace hope {
    namespace rtc {

        AudioDeviceModuleImpl::AudioDeviceModuleImpl()
            : audioHandle(nullptr),
            recording(false),
            playing(false),
            initialized(false),
            speakerInitialized(false),
            microphoneInitialized(false),
            recordingInitialized(false),
            playoutInitialized(false),
            stereoRecording(false) {
        }

        AudioDeviceModuleImpl::~AudioDeviceModuleImpl() {
            Terminate();
        }

        webrtc::scoped_refptr<AudioDeviceModuleImpl> AudioDeviceModuleImpl::Create() {
            return webrtc::scoped_refptr<AudioDeviceModuleImpl>(
                new webrtc::RefCountedObject<hope::rtc::AudioDeviceModuleImpl>());
        }

        void AudioDeviceModuleImpl::PushAudioData(unsigned char* data, size_t size) {
            if (!recording || !audioHandle) {
                return;
            }

            // WebRTC expects audio in specific format:
            // - 16-bit PCM samples
            // - 10ms chunks
            // - Specific sample rates (8000, 16000, 32000, 48000 Hz)

            const int sampleRate = 48000; // You can make this configurable
            const int channels = 2; // Mono audio, change to 2 for stereo
            const int bitsPerSample = 16;
            const int samplesPerChannel = sampleRate / 100; // 10ms of audio
            const size_t expectedSize = samplesPerChannel * channels * (bitsPerSample / 8);

            // Add data to buffer
            const uint8_t* audioData = reinterpret_cast<const uint8_t*>(data);
            audioBuffer.insert(audioBuffer.end(), audioData, audioData + size);

            // Process complete 10ms chunks
            while (audioBuffer.size() >= expectedSize) {
                // Extract 10ms chunk
                std::vector<uint8_t> chunk(audioBuffer.begin(), audioBuffer.begin() + expectedSize);
                audioBuffer.erase(audioBuffer.begin(), audioBuffer.begin() + expectedSize);

                // Variables for WebRTC callback
                uint32_t newMicLevel = 0;
                int64_t estimatedCaptureTimeNS = 0; // You might want to calculate this properly

                // Call WebRTC audio callback
                int32_t result = audioHandle->RecordedDataIsAvailable(
                    chunk.data(),
                    samplesPerChannel,
                    bitsPerSample / 8, // bytes per sample
                    channels,
                    sampleRate,
                    0, // total delay in ms
                    0, // clock drift
                    0, // current mic level
                    false, // key pressed
                    newMicLevel,
                    estimatedCaptureTimeNS
                );

                if (result != 0) {
                    LOG_ERROR("AudioHandle failed with result: %d", result);
                }
            }
        }

        void AudioDeviceModuleImpl::PushPcmFrame(const int16_t* samples, size_t numSamples, int sampleRate, int channels) {
            if (!recording || !audioHandle) {
                return;
            }

            const int samplesPerChannel = numSamples / channels;
            uint32_t newMicLevel = 0;

            int32_t result = audioHandle->RecordedDataIsAvailable(
                samples,
                samplesPerChannel,
                2, // bytes per sample (16-bit)
                channels,
                sampleRate,
                0, // total delay
                0, // clock drift
                0, // current mic level
                false, // key pressed
                newMicLevel,
                0  // estimated capture time
            );

            if (result != 0) {
                LOG_ERROR("AudioCallback failed");
            }
        }

        int32_t AudioDeviceModuleImpl::RegisterAudioCallback(webrtc::AudioTransport* audioCallback) {
            this->audioHandle = audioCallback;
            return 0;
        }

        void AudioDeviceModuleImpl::AddRef() const {}

        webrtc::RefCountReleaseStatus AudioDeviceModuleImpl::Release() const {
            return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
        }

        int32_t AudioDeviceModuleImpl::Init() {
            initialized = true;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::Terminate() {
            StopRecording();
            StopPlayout();

            initialized = false;
            return 0;
        }

        bool AudioDeviceModuleImpl::Initialized() const {
            return initialized;
        }

        int16_t AudioDeviceModuleImpl::PlayoutDevices() {
            return 1;
        }

        int16_t AudioDeviceModuleImpl::RecordingDevices() {
            return 1;
        }

        int32_t AudioDeviceModuleImpl::PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
            char guid[webrtc::kAdmMaxGuidSize]) {
            if (index != 0) return -1;
            strcpy_s(name, webrtc::kAdmMaxDeviceNameSize, "Virtual Playout Device");
            strcpy_s(guid, webrtc::kAdmMaxGuidSize, "virtual-playout-guid");
            return 0;
        }

        int32_t AudioDeviceModuleImpl::RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
            char guid[webrtc::kAdmMaxGuidSize]) {
            if (index != 0) return -1;
            strcpy_s(name, webrtc::kAdmMaxDeviceNameSize, "Virtual Recording Device");
            strcpy_s(guid, webrtc::kAdmMaxGuidSize, "virtual-recording-guid");
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetPlayoutDevice(uint16_t index) {
            return (index == 0) ? 0 : -1;
        }

        int32_t AudioDeviceModuleImpl::SetPlayoutDevice(WindowsDeviceType device) {
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetRecordingDevice(uint16_t index) {
            return (index == 0) ? 0 : -1;
        }

        int32_t AudioDeviceModuleImpl::SetRecordingDevice(WindowsDeviceType device) {
            return 0;
        }

        int32_t AudioDeviceModuleImpl::PlayoutIsAvailable(bool* available) {
            *available = true;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::InitPlayout() {
            playoutInitialized = true;
            return 0;
        }

        bool AudioDeviceModuleImpl::PlayoutIsInitialized() const {
            return playoutInitialized;
        }

        int32_t AudioDeviceModuleImpl::StartPlayout() {
            playing = true;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::StopPlayout() {
            playing = false;
            return 0;
        }

        bool AudioDeviceModuleImpl::Playing() const {
            return playing;
        }

        int32_t AudioDeviceModuleImpl::RecordingIsAvailable(bool* available) {
            *available = true;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::InitRecording() {
            recordingInitialized = true;
            return 0;
        }

        bool AudioDeviceModuleImpl::RecordingIsInitialized() const {
            return recordingInitialized;
        }

        int32_t AudioDeviceModuleImpl::StartRecording() {
            recording = true;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::StopRecording() {
            recording = false;
            audioBuffer.clear();
            return 0;
        }

        bool AudioDeviceModuleImpl::Recording() const {
            return recording;
        }

        int32_t AudioDeviceModuleImpl::InitSpeaker() {
            speakerInitialized = true;
            return 0;
        }

        bool AudioDeviceModuleImpl::SpeakerIsInitialized() const {
            return speakerInitialized;
        }

        int32_t AudioDeviceModuleImpl::InitMicrophone() {
            microphoneInitialized = true;
            return 0;
        }

        bool AudioDeviceModuleImpl::MicrophoneIsInitialized() const {
            return microphoneInitialized;
        }

        int32_t AudioDeviceModuleImpl::SpeakerVolumeIsAvailable(bool* available) {
            *available = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetSpeakerVolume(uint32_t volume) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::SpeakerVolume(uint32_t* volume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MaxSpeakerVolume(uint32_t* maxVolume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MinSpeakerVolume(uint32_t* minVolume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MicrophoneVolumeIsAvailable(bool* available) {
            *available = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetMicrophoneVolume(uint32_t volume) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MicrophoneVolume(uint32_t* volume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MaxMicrophoneVolume(uint32_t* maxVolume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MinMicrophoneVolume(uint32_t* minVolume) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::SpeakerMuteIsAvailable(bool* available) {
            *available = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetSpeakerMute(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::SpeakerMute(bool* enabled) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MicrophoneMuteIsAvailable(bool* available) {
            *available = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetMicrophoneMute(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::MicrophoneMute(bool* enabled) const {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::StereoPlayoutIsAvailable(bool* available) const {
            *available = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetStereoPlayout(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::StereoPlayout(bool* enabled) const {
            *enabled = false;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::StereoRecordingIsAvailable(bool* available) const {
            *available = true; // We can support stereo
            return 0;
        }

        int32_t AudioDeviceModuleImpl::SetStereoRecording(bool enable) {
            stereoRecording = enable;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::StereoRecording(bool* enabled) const {
            *enabled = stereoRecording;
            return 0;
        }

        int32_t AudioDeviceModuleImpl::PlayoutDelay(uint16_t* delayMS) const {
            *delayMS = 0;
            return 0;
        }

        bool AudioDeviceModuleImpl::BuiltInAECIsAvailable() const {
            return false;
        }

        bool AudioDeviceModuleImpl::BuiltInAGCIsAvailable() const {
            return false;
        }

        bool AudioDeviceModuleImpl::BuiltInNSIsAvailable() const {
            return false;
        }

        int32_t AudioDeviceModuleImpl::EnableBuiltInAEC(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::EnableBuiltInAGC(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::EnableBuiltInNS(bool enable) {
            return -1;
        }

        int32_t AudioDeviceModuleImpl::ActiveAudioLayer(AudioLayer* audioLayer) const {
            *audioLayer = AudioLayer::kDummyAudio;
            return 0;
        }

    } // namespace rtc
} // namespace hope
