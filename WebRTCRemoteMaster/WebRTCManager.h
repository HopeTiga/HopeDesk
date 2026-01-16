#pragma once
#ifndef WebRTCManager_H
#define WebRTCManager_H

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
#include <modules/audio_device/include/audio_device.h>
#include <api/enable_media_with_defaults.h>


#include "PeerConnectionObserverImpl.h"
#include "VideoTrackSinkImpl.h"
#include "AudioDeviceModuleImpl.h"
#include "DataChannelObserverImpl.h"
#include "SetDescriptionObserverImpl.h"
#include "CreateDescriptionObserverImpl.h"

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "MsquicSocketClient.h"
#include "WindowsServiceManager.h"
#include "Utils.h"
#include "concurrentqueue.h"


#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

namespace hope{

namespace rtc{

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
    START = 4,
    CLOSE = 5,
};

enum class ChannelType{

    Control = 0,

    Cursor = 1

};

struct VideoFrame {
    // 核心：持有 WebRTC 的 buffer 引用
    // scoped_refptr 会自动管理生命周期，引用计数归零时 WebRTC 会自动回收内存
    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer;

    int width = 0;
    int height = 0;

    VideoFrame() = default;

    // 构造函数：直接接管 WebRTC 的帧
    explicit VideoFrame(const webrtc::VideoFrame& frame) {
        // ToI420() 如果底层已经是 I420，这里只是指针转换，开销极低
        buffer = frame.video_frame_buffer()->ToI420();
        width = frame.width();
        height = frame.height();
    }
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


class WebRTCManager
{
    friend class PeerConnectionObserverImpl;
    friend class DataChannelObserverImpl;
    friend class VideoTrackSinkImpl;
public:
    WebRTCManager(WebRTCRemoteState state);

    ~WebRTCManager();

    WebRTCManager(const WebRTCManager&) = delete;

    WebRTCManager& operator=(const WebRTCManager&) = delete;

public:
    void sendRequestToTarget(int webrtcModulesType = 0,int webrtcUseGPU = 0,int videoCodec = 4,int webrtcAudioEnable = 0);

    void connect(std::string ip);

    void sendSignalingMessage(boost::json::object &  msg);

    void disConnect();

    using VideoFrameCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

    void setVideoFrameCallback(VideoFrameCallback callback);

    std::string getAccountId() const;

    void setAccountId(const std::string& newAccountID);

    std::string getTargetId() const;

    void setTargetId(const std::string& newTargetID);

    void writerRemote(unsigned char* data, size_t size,ChannelType  channelType);

    void writerAsync(std::shared_ptr<WriterData> writerData);

    void disConnectRemote();

    std::function<void(void)> msquicSocketDisConnect;

    std::function<void(void)> followRemoteHandle;

    std::function<void(void)> disConnectRemoteHandle;

    std::function<void(void)> remoteSuccessFulHandle;

    std::function<void(bool)> msquicSocketConnectedHandle;

    std::function<void()> remoteFailedHandle;

    void disConnectHandle();

    void setSystemServiceExe(std::string webrtcExe);

private:
    // 状态变量使用原子类型
    std::atomic<WebRTCRemoteState> state;

    std::atomic<WebRTCConnetState> connetState;

    hope::quic::MsquicSocketClient * msquicSocketClient;

    std::string accountId;

    std::string targetId;

    std::unique_ptr<webrtc::Thread> networkThread;

    std::unique_ptr<webrtc::Thread> workerThread;

    std::unique_ptr<webrtc::Thread> signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peerConnectionFactory;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> mouseMoveDataChannel;

    webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> videoSender;

    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack;

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> audioSender;

    std::unique_ptr<PeerConnectionObserverImpl> peerConnectionObserver;

    std::unique_ptr<DataChannelObserverImpl> dataChannelObserver;

    std::unique_ptr<DataChannelObserverImpl> mouseMoveDataChannelObserver;

    webrtc::scoped_refptr<CreateOfferObserverImpl> createOfferObserver;

    webrtc::scoped_refptr<CreateAnswerObserverImpl> createAnswerObserver;

    std::unique_ptr<VideoTrackSinkImpl> videoSinkImpl;

    std::atomic<bool> isInit{false};

private:

    // 发送端功能
    bool initializePeerConnection();

    void convertYUV420ToRGBA32(const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
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

    boost::asio::steady_timer steadyTimer;

    boost::asio::ip::tcp::acceptor accept;

    std::queue<std::vector<std::byte>> remoteBinaryQueue;

    std::function<void(std::shared_ptr<VideoFrame>)> videoFrameCallback;

    std::queue<std::vector<std::byte>> trackFrameQueues;

    std::unique_ptr<boost::asio::ip::tcp::socket> tcpSocket;

    boost::asio::experimental::channel<void(boost::system::error_code)> writerChannel;

    moodycamel::ConcurrentQueue<std::shared_ptr<WriterData>> writerDataQueues{ 1 };

    std::atomic<bool> socketRuns{false};

    std::string followData;

    std::string systemService = "WebRTCSystemLogon";

    std::string systemServiceExe;

    std::vector<std::vector<unsigned char>> cursorArray ;

    std::string dataStr;

};


}

}

#endif // WebRTCManager_H
