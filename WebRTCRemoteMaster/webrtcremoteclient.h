#pragma once
#ifndef WEBRTCREMOTECLIENT_H
#define WEBRTCREMOTECLIENT_H

#define WEBRTC_WIN 1

#ifdef _WIN32
#include <windows.h>
#endif

#include <thread>
#include <memory>
#include <queue>
#include <functional>
#include <atomic>
#include <coroutine>

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

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "WindowsServiceManager.h"
#include "Utils.h"
#include "concurrentqueue.h"


#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

enum class WebRTCRemoteState{
    nullRemote = 0,
    masterRemote = 1,  // 接收端：接收并显示视频流
    followerRemote = 2, // 发送端：捕获屏幕并发送视频流
};

enum class WebRTCConnetState{
    none,
    connect,
};

enum class WebRTCRequestState {
    REGISTER = 0,
    REQUEST = 1,
    RESTART = 2,
    STOPREMOTE = 3,
    CLOSE = 4,
};

struct VideoFrame {
    std::shared_ptr<uint8_t[]> data;
    int width;
    int height;
    int64_t timestamp;

    VideoFrame(int w, int h) : width(w), height(h), timestamp(0) {
        data = std::make_shared<uint8_t[]>(width * height * 3);
    }

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) = default;
    VideoFrame& operator=(VideoFrame&&) = default;
};

class WriterData {
public:
    WriterData(const char* data, size_t size) : size(size) {
        this->data = new char[size + sizeof(int64_t)];

        uint64_t size64t = boost::asio::detail::socket_ops::network_to_host_long(
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

// Forward declarations
class WebRTCRemoteClient;

class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
public:
    explicit PeerConnectionObserverImpl(WebRTCRemoteClient* client) : client(client) {}

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
                             int errorCode, const std::string& errorText) override {}
    void OnIceCandidateRemoved(const webrtc::IceCandidate* candidate) override {}
    void OnIceCandidatesRemoved(const std::vector<webrtc::Candidate>& candidates) override {}
    void OnIceConnectionReceivingChange(bool receiving) override {}
    void OnIceSelectedCandidatePairChanged(const webrtc::CandidatePairChangeEvent& event) override {}
    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {}
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
    void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {}
    void OnInterestingUsage(int usagePattern) override {}

private:
    WebRTCRemoteClient* client;
};

class DataChannelObserverImpl : public webrtc::DataChannelObserver {
public:
    DataChannelObserverImpl(WebRTCRemoteClient* client) : client(client) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
private:
    WebRTCRemoteClient* client;
};

// 在 webrtcremoteclient.h 中添加
class VideoTrackSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit VideoTrackSink(WebRTCRemoteClient* client) : client(client) {}

    // 接收视频帧的回调
    void OnFrame(const webrtc::VideoFrame& frame) override ;

private:
    WebRTCRemoteClient* client;
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
        WebRTCRemoteClient* client,
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        return webrtc::scoped_refptr<CreateOfferObserverImpl>(
            new webrtc::RefCountedObject<CreateOfferObserverImpl>(client, pc));
    }

    CreateOfferObserverImpl(WebRTCRemoteClient* client,
                            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
        : client(client), peerConnection(pc) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

protected:
    ~CreateOfferObserverImpl() override = default;

private:
    WebRTCRemoteClient* client;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
};

class CreateAnswerObserverImpl : public webrtc::CreateSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<CreateAnswerObserverImpl> Create(
        WebRTCRemoteClient* client,
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        return webrtc::scoped_refptr<CreateAnswerObserverImpl>(
            new webrtc::RefCountedObject<CreateAnswerObserverImpl>(client, pc));
    }

    CreateAnswerObserverImpl(WebRTCRemoteClient* client,
                             webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc)
        : client(client), peerConnection(pc) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

protected:
    ~CreateAnswerObserverImpl() override = default;

private:
    WebRTCRemoteClient* client;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;
};

class WebRTCRemoteClient
{
    friend class PeerConnectionObserverImpl;
    friend class DataChannelObserverImpl;
    friend class VideoTrackSink;
public:
    WebRTCRemoteClient(WebRTCRemoteState state);

    ~WebRTCRemoteClient();

    WebRTCRemoteClient(const WebRTCRemoteClient&) = delete;

    WebRTCRemoteClient& operator=(const WebRTCRemoteClient&) = delete;

public:
    void sendRequestToTarget();

    void connect(std::string ip);

    void sendSignalingMessage(boost::json::object &  msg);

    void disConnect();

    using VideoFrameCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

    void setVideoFrameCallback(VideoFrameCallback callback);

    std::string getAccountID() const;

    void setAccountID(const std::string& newAccountID);

    std::string getTargetID() const;

    void setTargetID(const std::string& newTargetID);

    void writerRemote(unsigned char* data, size_t size);

    void writerAsync(std::shared_ptr<WriterData> writerData);

    void disConnectRemote();

    std::function<void(void)> followRemoteHandle;

    std::function<void(void)> disConnectRemoteHandle;

    std::function<void(void)> remoteSuccessFulHandle;

    std::function<void(bool)> webSocketConnectedCallback;

    std::function<void()> remoteFailedHandle;

    void disConnectHandle();

private:
    // 状态变量使用原子类型
    std::atomic<WebRTCRemoteState> state;

    std::atomic<WebRTCConnetState> connetState;

    std::unique_ptr<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>> webSocket;

    std::unique_ptr<boost::asio::ip::tcp::resolver> resolver;

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

    std::unique_ptr<VideoTrackSink> videoSink;

    std::atomic<bool> isInit{false};

    std::atomic<bool> webSocketRuns {false};

private:

    // 发送端功能
    bool initializePeerConnection();

    void convertYUV420ToRGB24(const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
                              int width, int height, int yStride, int uStride, int vStride,
                              uint8_t* rgbData);

    void wrtierCoroutineAsync();

    void receiveCoroutineAysnc();

    void handleAsioException();

    void releaseSource();

    std::atomic<bool> followRunning{false};

    std::atomic<bool> shouldStop{false};

    std::atomic<bool> isStreaming;

    // 视频参数
    std::atomic<int> videoWidth{1920};

    std::atomic<int> videoHeight{1080};

    std::atomic<bool> isRemote {false};

    // 服务状态
    std::atomic<bool> remoteServiceRunning{false};

    boost::asio::io_context ioContext;

    boost::asio::experimental::channel<void(boost::system::error_code)> channel;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

    std::thread ioContextThread;

    std::queue<std::vector<std::byte>> remoteBinaryQueue;

    std::function<void(std::shared_ptr<VideoFrame>)> videoFrameCallback;

    std::atomic<bool> isReceive {false};

    std::queue<std::vector<std::byte>> trackFrameQueues;

    std::unique_ptr<boost::asio::ip::tcp::socket> tcpSocket;

    boost::asio::experimental::channel<void(boost::system::error_code)> writerChannel;

    moodycamel::ConcurrentQueue<std::shared_ptr<WriterData>> writerDataQueues{ 1 };

    std::atomic<bool> socketRuns{false};

    std::string followData;

    std::string systemService = "WebRTCSystemLogon";

    std::string systemServiceExe = "E:\\cppPro\\WebRTCSystemLogon-version\\x64\\Release\\WebRTCSystemLogon.exe";
};

#endif // WEBRTCREMOTECLIENT_H
