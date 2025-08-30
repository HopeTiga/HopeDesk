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

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.Preview.Injection.h>
#include <winrt/Windows.System.h>

// Project includes
#include "concurrentqueue.h"
#include "ScreenCapture.h"
#include "WinLogon.h"
#include "KeyMouseSimulator.h"
#include "Utils.h"

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
    STOPREMOTE = 3
};

class WriterData {
public:
    WriterData(char* data, size_t size) : size(size) {
        this->data = new char[size + sizeof(int64_t)];
        uint64_t size64t = boost::asio::detail::socket_ops::host_to_network_long(
            static_cast<uint64_t>(size));
        std::memcpy(this->data, &size64t, sizeof(uint64_t));
        std::memcpy(this->data + sizeof(uint64_t), data, size);
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
    explicit PeerConnectionObserverImpl(WebRTCManager* manager) : manager_(manager) {}

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {}
    void OnRenegotiationNeeded() override {}
    void OnNegotiationNeededEvent(uint32_t event_id) override {}
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
    void OnIceCandidateError(const std::string& address, int port, const std::string& url,
        int error_code, const std::string& error_text) override {
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
    void OnInterestingUsage(int usage_pattern) override {}

private:
    WebRTCManager* manager_;
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
        : manager_(manager), peerConnection(pc) {
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

protected:
    ~CreateOfferObserverImpl() override = default;

private:
    WebRTCManager* manager_;
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
        : manager_(manager), peerConnection(pc) {
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

protected:
    ~CreateAnswerObserverImpl() override = default;

private:
    WebRTCManager* manager_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
};

class WebRTCManager {
    friend class DataChannelObserverImpl;
public:
    WebRTCManager(WebRTCRemoteState state);
    ~WebRTCManager();
    void Cleanup();

    // 发送信令消息
    void sendSignalingMessage(const boost::json::object& message);
    void processOffer(const std::string& sdp);
    void processAnswer(const std::string& sdp);
    void processIceCandidate(const std::string& candidate, const std::string& mid, int lineIndex);

    void writerAsync(std::shared_ptr<WriterData> data);

    // Add releaseSource method declaration
    void releaseSource();

public:

    std::function<void(void)> stopProcessCallBack;

    std::atomic<bool> isProcessingOffer{ false }; // Add flag to prevent simultaneous offers

private:

    void socketEventLoop();

    bool initializePeerConnection();

    bool initializeScreenCapture();

    void processDataChannelMessage(const std::vector<std::byte>& bytes);

private:

    std::string accountID;

    std::string targetID;

    std::unique_ptr<webrtc::Thread> networkThread;
    std::unique_ptr<webrtc::Thread> workerThread;
    std::unique_ptr<webrtc::Thread> signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peerConnectionFactory;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> videoSender;

    std::unique_ptr<PeerConnectionObserverImpl> peerConnectionObserver;
    std::unique_ptr<DataChannelObserverImpl> dataChannelObserver;
    webrtc::scoped_refptr<CreateOfferObserverImpl> createOfferObserver;
    webrtc::scoped_refptr<CreateAnswerObserverImpl> createAnswerObserver;
    webrtc::scoped_refptr<VideoTrackSourceImpl> videoTrackSourceImpl;

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

    winrt::Windows::UI::Input::Preview::Injection::InputInjector inputInjector;

    std::unique_ptr<KeyMouseSimulator> keyMouseSim;

};