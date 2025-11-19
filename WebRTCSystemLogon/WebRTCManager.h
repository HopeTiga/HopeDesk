#pragma once
#define WEBRTC_WIN 1
#define WEBRTC_ARCH_LITTLE_ENDIAN 1
#define NOMINMAX 1
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4146)
#pragma warning(disable: 4996)
#endif

#include <thread>
#include <memory>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <unordered_map>
#include <chrono>
#include <deque>
#include <functional>
#include <atomic>

#include <api/peer_connection_interface.h>
#include <api/create_peerconnection_factory.h>
#include <api/data_channel_interface.h>
#include <api/media_stream_interface.h>
#include <api/rtp_receiver_interface.h>
#include <api/rtp_transceiver_interface.h>
#include <api/jsep.h>
#include <api/rtc_error.h>
#include <api/scoped_refptr.h>
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/audio/audio_mixer.h>
#include <api/audio/audio_processing.h>
#include <rtc_base/thread.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/ref_counted_object.h>
#include <pc/video_track_source.h>
#include <modules/video_capture/video_capture_factory.h>
#include <modules/audio_device/include/audio_device.h>
#include <api/enable_media_with_defaults.h>
#include <media/base/adapted_video_track_source.h>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json.hpp>

// Project includes
#include "concurrentqueue.h"
#include "ScreenCapture.h"
#include "WinLogon.h"
#include "KeyMouseSimulator.h"
#include "CursorHooks.h"
#include "Utils.h"

namespace hope {

	namespace rtc {
	
        enum class WebRTCRemoteState {
            nullRemote = 0,
            masterRemote = 1,
            followerRemote = 2,
        };

        enum class WebRTCConnetState {
            none,
            connect,
        };

        enum class WebRTCRequestState {
            REGISTER = 0,
            REQUEST = 1,
            RESTART = 2,
            STOPREMOTE = 3,
            START = 4,
            CLOSE = 5,
        };

        enum class WebRTCVideoCodec {
            VP8,    // VP8 ±à½âÂëÆ÷
            VP9,    // VP9 ±à½âÂëÆ÷
            H264,    // H.264 ±à½âÂëÆ÷
            H265,    // H.265 ±à½âÂëÆ÷
            AV1, // AV1 ±à½âÂëÆ÷    
        };

        class WriterData {

        public:

            WriterData(char* data, size_t size) : size(size) {

                this->data = new char[size + sizeof(int64_t)];

                uint64_t size64t = boost::asio::detail::socket_ops::host_to_network_long(
                    static_cast<uint64_t>(size));

                fastCopy(this->data, &size64t, sizeof(uint64_t));

                fastCopy(this->data + sizeof(uint64_t), data, size);

                this->size = size + sizeof(int64_t);

            };

            ~WriterData() {

                if (data != nullptr) {

                    delete[] data;

                    data = nullptr;

                }
            }

            char* data;

            size_t size;
        };

        // Forward declaration
        class WebRTCManager;

        class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {

        public:

            explicit PeerConnectionObserverImpl(WebRTCManager* manager) : manager(manager) {}

            void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) override;

            void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) override;

            void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) override;

            void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;

            void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}

            void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}

            void OnRenegotiationNeeded() override {}

            void OnNegotiationNeededEvent(uint32_t eventId) override {}

            void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) override;

            void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) override {}

            void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) override;

            void OnIceCandidateError(const std::string& address, int port, const std::string& url,
                int errorCode, const std::string& errorText) override {
            }
            void OnIceCandidateRemoved(const webrtc::IceCandidate* candidate) override {}

            void OnIceCandidatesRemoved(const std::vector<webrtc::Candidate>& candidates) override {}

            void OnIceConnectionReceivingChange(bool receiving) override {}

            void OnIceSelectedCandidatePairChanged(const webrtc::CandidatePairChangeEvent& event) override {}

            void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {
            }

            void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {}

            void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {}

            void OnInterestingUsage(int usagePattern) override {}

        private:

            WebRTCManager* manager;

        };

        class DataChannelObserverImpl : public webrtc::DataChannelObserver {

        public:

            DataChannelObserverImpl(WebRTCManager* manager) :manager(manager) {
            }

            void OnStateChange() override {}

            void OnMessage(const webrtc::DataBuffer& buffer) override;

        private:

            WebRTCManager* manager;

        };

        class VideoTrackSourceImpl : public webrtc::AdaptedVideoTrackSource {

        public:

            VideoTrackSourceImpl() : AdaptedVideoTrackSource(1) {}

            ~VideoTrackSourceImpl() override = default;

            SourceState state() const override { return kLive; }

            bool remote() const override { return false; }

            bool is_screencast() const override { return true; }

            std::optional<bool> needs_denoising() const override { return false; }

            void PushFrame(const webrtc::VideoFrame& frame) {

                OnFrame(frame);

            }
        };


        class AudioDeviceModuleImpl : public webrtc::AudioDeviceModule {
        public:
            AudioDeviceModuleImpl()
                : audioCallback(nullptr),
                recording(false),
                playing(false),
                initialized(false),
                speakerInitialized(false),
                microphoneInitialized(false),
                recordingInitialized(false),
                playoutInitialized(false) {
            }

            ~AudioDeviceModuleImpl() {
                Terminate();
            }

            static webrtc::scoped_refptr<AudioDeviceModuleImpl> Create() {
                return webrtc::scoped_refptr<AudioDeviceModuleImpl>(
                    new webrtc::RefCountedObject<AudioDeviceModuleImpl>());
            }

            // Key method: Push audio data into WebRTC
            void PushAudioData(unsigned char* data, size_t size) {
                if (!recording || !audioCallback) {
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
                    size_t nSamplesOut = 0;
                    int64_t estimatedCaptureTimeNS = 0; // You might want to calculate this properly

                    // Call WebRTC audio callback
                    int32_t result = audioCallback->RecordedDataIsAvailable(
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
                        Logger::getInstance()->error("AudioCallback failed with result: " + std::to_string(result));
                    }
                }
            }

            // Alternative method for pushing raw PCM frames directly
            void PushPCMFrame(const int16_t* samples, size_t numSamples, int sampleRate, int channels) {
                if (!recording || !audioCallback) {
                    return;
                }

                const int samplesPerChannel = numSamples / channels;
                uint32_t newMicLevel = 0;

                int32_t result = audioCallback->RecordedDataIsAvailable(
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
                    Logger::getInstance()->error("AudioCallback failed");
                }
            }

            // Register audio callback - this is called by WebRTC
            int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
                this->audioCallback = audioCallback;
                return 0;
            }

            // RefCounted interface
            void AddRef() const override {}
            webrtc::RefCountReleaseStatus Release() const override {
                return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
            }

            // Initialization methods
            int32_t Init() override {
                initialized = true;
                return 0;
            }

            int32_t Terminate() override {
                StopRecording();
                StopPlayout();

                initialized = false;
                return 0;
            }

            bool Initialized() const override {
                return initialized;
            }

            // Device enumeration (stub implementations for virtual device)
            int16_t PlayoutDevices() override { return 1; }
            int16_t RecordingDevices() override { return 1; }

            int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                char guid[webrtc::kAdmMaxGuidSize]) override {
                if (index != 0) return -1;
                strcpy_s(name, webrtc::kAdmMaxDeviceNameSize, "Virtual Playout Device");
                strcpy_s(guid, webrtc::kAdmMaxGuidSize, "virtual-playout-guid");
                return 0;
            }

            int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                char guid[webrtc::kAdmMaxGuidSize]) override {
                if (index != 0) return -1;
                strcpy_s(name, webrtc::kAdmMaxDeviceNameSize, "Virtual Recording Device");
                strcpy_s(guid, webrtc::kAdmMaxGuidSize, "virtual-recording-guid");
                return 0;
            }

            // Device selection
            int32_t SetPlayoutDevice(uint16_t index) override { return (index == 0) ? 0 : -1; }
            int32_t SetPlayoutDevice(WindowsDeviceType device) override { return 0; }
            int32_t SetRecordingDevice(uint16_t index) override { return (index == 0) ? 0 : -1; }
            int32_t SetRecordingDevice(WindowsDeviceType device) override { return 0; }

            // Playout methods
            int32_t PlayoutIsAvailable(bool* available) override {
                *available = true;
                return 0;
            }

            int32_t InitPlayout() override {
                playoutInitialized = true;
                return 0;
            }

            bool PlayoutIsInitialized() const override {
                return playoutInitialized;
            }

            int32_t StartPlayout() override {
                playing = true;
                return 0;
            }

            int32_t StopPlayout() override {
                playing = false;
                return 0;
            }

            bool Playing() const override {
                return playing;
            }

            // Recording methods
            int32_t RecordingIsAvailable(bool* available) override {
                *available = true;
                return 0;
            }

            int32_t InitRecording() override {
                recordingInitialized = true;
                return 0;
            }

            bool RecordingIsInitialized() const override {
                return recordingInitialized;
            }

            int32_t StartRecording() override {
                recording = true;
                return 0;
            }

            int32_t StopRecording() override {
                recording = false;

                audioBuffer.clear();

                return 0;
            }

            bool Recording() const override {
                return recording;
            }

            // Speaker/Microphone initialization
            int32_t InitSpeaker() override {
                speakerInitialized = true;
                return 0;
            }

            bool SpeakerIsInitialized() const override {
                return speakerInitialized;
            }

            int32_t InitMicrophone() override {
                microphoneInitialized = true;
                return 0;
            }

            bool MicrophoneIsInitialized() const override {
                return microphoneInitialized;
            }

            // Volume controls (stub implementations)
            int32_t SpeakerVolumeIsAvailable(bool* available) override {
                *available = false;
                return 0;
            }

            int32_t SetSpeakerVolume(uint32_t volume) override { return -1; }
            int32_t SpeakerVolume(uint32_t* volume) const override { return -1; }
            int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override { return -1; }
            int32_t MinSpeakerVolume(uint32_t* minVolume) const override { return -1; }

            int32_t MicrophoneVolumeIsAvailable(bool* available) override {
                *available = false;
                return 0;
            }

            int32_t SetMicrophoneVolume(uint32_t volume) override { return -1; }
            int32_t MicrophoneVolume(uint32_t* volume) const override { return -1; }
            int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const override { return -1; }
            int32_t MinMicrophoneVolume(uint32_t* minVolume) const override { return -1; }

            // Mute controls
            int32_t SpeakerMuteIsAvailable(bool* available) override {
                *available = false;
                return 0;
            }

            int32_t SetSpeakerMute(bool enable) override { return -1; }
            int32_t SpeakerMute(bool* enabled) const override { return -1; }

            int32_t MicrophoneMuteIsAvailable(bool* available) override {
                *available = false;
                return 0;
            }

            int32_t SetMicrophoneMute(bool enable) override { return -1; }
            int32_t MicrophoneMute(bool* enabled) const override { return -1; }

            // Stereo support
            int32_t StereoPlayoutIsAvailable(bool* available) const override {
                *available = false;
                return 0;
            }

            int32_t SetStereoPlayout(bool enable) override { return -1; }
            int32_t StereoPlayout(bool* enabled) const override {
                *enabled = false;
                return 0;
            }

            int32_t StereoRecordingIsAvailable(bool* available) const override {
                *available = true; // We can support stereo
                return 0;
            }

            int32_t SetStereoRecording(bool enable) override {
                stereoRecording = enable;
                return 0;
            }

            int32_t StereoRecording(bool* enabled) const override {
                *enabled = stereoRecording;
                return 0;
            }

            // Delay
            int32_t PlayoutDelay(uint16_t* delayMS) const override {
                *delayMS = 0;
                return 0;
            }

            // Built-in audio processing
            bool BuiltInAECIsAvailable() const override { return false; }
            bool BuiltInAGCIsAvailable() const override { return false; }
            bool BuiltInNSIsAvailable() const override { return false; }

            int32_t EnableBuiltInAEC(bool enable) override { return -1; }
            int32_t EnableBuiltInAGC(bool enable) override { return -1; }
            int32_t EnableBuiltInNS(bool enable) override { return -1; }

            // Active audio layer
            int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override {
                *audioLayer = AudioLayer::kDummyAudio;
                return 0;
            }

        private:
            webrtc::AudioTransport* audioCallback;
            std::atomic<bool> recording;
            std::atomic<bool> playing;
            std::atomic<bool> initialized;
            std::atomic<bool> speakerInitialized;
            std::atomic<bool> microphoneInitialized;
            std::atomic<bool> recordingInitialized;
            std::atomic<bool> playoutInitialized;
            std::atomic<bool> stereoRecording{ false };

            // Audio buffer for accumulating data
            std::vector<uint8_t> audioBuffer;

        };

        class SetLocalDescriptionObserver : public webrtc::SetSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<SetLocalDescriptionObserver> Create() {

                return webrtc::scoped_refptr<SetLocalDescriptionObserver>(
                    new webrtc::RefCountedObject<SetLocalDescriptionObserver>());

            }

            void OnSuccess() override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            SetLocalDescriptionObserver() = default;

            ~SetLocalDescriptionObserver() override = default;

        };

        class SetRemoteDescriptionObserver : public webrtc::SetSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<SetRemoteDescriptionObserver> Create() {

                return webrtc::scoped_refptr<SetRemoteDescriptionObserver>(
                    new webrtc::RefCountedObject<SetRemoteDescriptionObserver>());

            }

            void OnSuccess() override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            SetRemoteDescriptionObserver() = default;

            ~SetRemoteDescriptionObserver() override = default;

        };

        class CreateOfferObserverImpl : public webrtc::CreateSessionDescriptionObserver {

        public:

            static webrtc::scoped_refptr<CreateOfferObserverImpl> Create(
                WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateOfferObserverImpl>(
                    new webrtc::RefCountedObject<CreateOfferObserverImpl>(manager, pc));

            }

            CreateOfferObserverImpl(WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : manager(manager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateOfferObserverImpl() override = default;

        private:
            WebRTCManager* manager;


            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

        class CreateAnswerObserverImpl : public webrtc::CreateSessionDescriptionObserver {
        public:
            static webrtc::scoped_refptr<CreateAnswerObserverImpl> Create(
                WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {

                return webrtc::scoped_refptr<CreateAnswerObserverImpl>(
                    new webrtc::RefCountedObject<CreateAnswerObserverImpl>(manager, pc));

            }

            CreateAnswerObserverImpl(WebRTCManager* manager,
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
                : manager(manager), peerConnection(pc) {
            }

            void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;

            void OnFailure(webrtc::RTCError error) override;

        protected:

            ~CreateAnswerObserverImpl() override = default;

        private:

            WebRTCManager* manager;

            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

        };

        class WebRTCManager {

            friend class PeerConnectionObserverImpl;

            friend class DataChannelObserverImpl;

        public:
            WebRTCManager(WebRTCVideoCodec codec = WebRTCVideoCodec::AV1, webrtc::RtpEncodingParameters rtpEncodingParameters = getDefaultRtpEncodingParameters());

            ~WebRTCManager();

            void Cleanup();

            void sendSignalingMessage(const boost::json::object& message);

            void processOffer(const std::string& sdp);

            void processAnswer(const std::string& sdp);

            void processIceCandidate(const std::string& candidate, const std::string& mid, int lineIndex);

            void writerAsync(std::shared_ptr<WriterData> data);

            void releaseSource();

        public:

            std::function<void(void)> stopProcessCallBack;

            std::atomic<bool> isProcessingOffer{ false }; // Add flag to prevent simultaneous offers

        private:

            void socketEventLoop();

            bool initializePeerConnection();

            bool initializeScreenCapture();

            void processDataChannelMessage(const unsigned char* data, size_t size);

            static webrtc::RtpEncodingParameters getDefaultRtpEncodingParameters() {
                webrtc::RtpEncodingParameters encoding;
                encoding.active = true;
                encoding.max_bitrate_bps = 2000000;  // 4 Mbps
                encoding.min_bitrate_bps = 2000000;  // 1 Mbps
                encoding.bitrate_priority = 4.0;
                encoding.max_framerate = 120;
                encoding.scale_resolution_down_by = 1.0;
                encoding.bitrate_priority = 1.0;
                encoding.scalability_mode = "L1T1";
                encoding.network_priority = webrtc::Priority::kHigh;
                return encoding;
            }

        private:

            std::string accountID;

            std::string targetID;

            WebRTCVideoCodec codec;

            webrtc::RtpEncodingParameters rtpEncodingParameters;

            std::unique_ptr<webrtc::Thread> networkThread;

            std::unique_ptr<webrtc::Thread> workerThread;

            std::unique_ptr<webrtc::Thread> signalingThread;

            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

            webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peerConnectionFactory;

            webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;

            webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;

            webrtc::scoped_refptr<webrtc::RtpSenderInterface> videoSender;

            webrtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack;

            std::unique_ptr<PeerConnectionObserverImpl> peerConnectionObserver;

            std::unique_ptr<DataChannelObserverImpl> dataChannelObserver;

            webrtc::scoped_refptr<CreateOfferObserverImpl> createOfferObserver;

            webrtc::scoped_refptr<CreateAnswerObserverImpl> createAnswerObserver;

            webrtc::scoped_refptr<VideoTrackSourceImpl> videoTrackSourceImpl;

            webrtc::scoped_refptr<AudioDeviceModuleImpl> audioDeviceModuleImpl;

            std::atomic<bool> isInit{ false };

            std::atomic<WebRTCRemoteState> state;

            std::atomic<WebRTCConnetState> connetState;

            std::shared_ptr<ScreenCapture> screenCapture;

            std::atomic<bool> socketRuns{ false };

            boost::asio::io_context ioContext;

            std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

            std::thread ioContextThread;

            boost::asio::io_context socketIoContext;

            std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> socketIoContextWorkPtr;

            std::thread socketIoContextThread;

            boost::asio::ip::tcp::acceptor accept;

            std::unique_ptr<boost::asio::ip::tcp::socket> tcpSocket;

            boost::asio::experimental::channel<void(boost::system::error_code)> writerChannel;

            moodycamel::ConcurrentQueue<std::shared_ptr<WriterData>> writerDataQueues{ 1 };

            std::unique_ptr<WinLogon> winLogon;

            std::unique_ptr<KeyMouseSimulator> keyMouseSim;

            std::unique_ptr<CursorHooks> cursorHooks;

        };

	}

}