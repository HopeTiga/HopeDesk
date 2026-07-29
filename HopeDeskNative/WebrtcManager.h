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

#include "WebrtcVideoEncoderFactory.h"
#include "WebrtcVideoDecoderFactory.h"
#include "PeerConnectionObserverImpl.h"
#include "VideoTrackSinkImpl.h"
#include "AudioDeviceModuleImpl.h"
#include "DataChannelObserverImpl.h"
#include "SetDescriptionObserverImpl.h"
#include "CreateDescriptionObserverImpl.h"
#include "RTCStatsCollectorHandle.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <boost/json.hpp>

#include "WindowsServiceManager.h"
#include "Utils.h"
#include "AsioConcurrentQueue.h"


#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

// 前向声明 D3D11 接口，避免在本头里拉入 <d3d11.h>
struct ID3D11Texture2D;

namespace hope{

namespace rtc{

enum class WebrtcRequestState {
    REGISTER = 0,
    REQUEST = 1,
    RESTART = 2,
    STOPREMOTE = 3,
    START = 4,
    CLOSE = 5,
    CLOSESYSTEM = 6,
    SYSTEMREADLY = 7,
    STATS = 8,
    ENCODE_STATUS = 9   // System -> 被控 Native:上报当前编码 codec + 硬编/软编
};


enum class FrameFormat {
    Unknown,
    I420,
    Nv12
};

struct VideoFrame {
    FrameFormat format = FrameFormat::I420;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer;

    webrtc::scoped_refptr<const webrtc::NV12BufferInterface> nv12Buffer;

    int width = 0;
    int height = 0;

    VideoFrame() = default;

    explicit VideoFrame(const webrtc::VideoFrame& frame) {
        buffer = frame.video_frame_buffer()->ToI420();
        width = frame.width();
        height = frame.height();
        format = FrameFormat::I420;
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

struct WebrtcDeskConfig {
    int webrtcModulesType = 0;   // 0=游戏模式 1=办公模式
    int webrtcUseLevels = 2;     // 加速策略/采集层级
    int videoCodec = 4;          // 视频编码索引
    int webrtcAudioEnable = 0;   // 是否传音频
    int webrtcEnableNvenc = 0;    // 硬件编码(NVENC),发给 System
    int webrtcEnableNvdec = 0;   // 硬件解码(MF/D3D11),Native 本地

    // 编码配置:两组(请求组 + 本地组),各含 最大码率/最小码率/最大帧率。
    // 码率单位 bps(与 RtpEncodingParameters 对齐),帧率 fps。
    // 请求组:随 REQUEST JSON 发给远端 System;本地组:仅本机持有,由本地 TCP 给本机 System。
    int requestMaxBitrateBps = 15000000;  // 请求组最大码率,默认 15 Mbps
    int requestMinBitrateBps = 15000000;  // 请求组最小码率,默认 15 Mbps
    int requestMaxFramerate  = 144;        // 请求组最大帧率
    int localMaxBitrateBps   = 15000000;  // 本地组最大码率,默认 15 Mbps
    int localMinBitrateBps   = 15000000;  // 本地组最小码率,默认 15 Mbps
    int localMaxFramerate    = 144;       // 本地组最大帧率
};


// (NvdecDecoder 仅在解码工厂内部使用,WebrtcManager 不直接引用)

class WebrtcManager : public std::enable_shared_from_this<WebrtcManager>
{
    friend class PeerConnectionObserverImpl;
    friend class DataChannelObserverImpl;
    friend class VideoTrackSinkImpl;
public:
    WebrtcManager();

    ~WebrtcManager();

    WebrtcManager(const WebrtcManager&) = delete;

    WebrtcManager& operator=(const WebrtcManager&) = delete;

public:

    void asyncEvent();

    void closeEvent();

    void asyncRemoteDesk(WebrtcDeskConfig webrtcDeskConfig);

    // 给对端发送 Ctrl+Alt+F 组合键(逐键 down/up 序列)
    void sendKeyComboCtrlAltF();

    // 重连时清空 cursor 缓存(新 datachannel 到来调用),使 index 与对端重新对齐
    void resetCursorCache();

    // 碰到 Invalid cursor index 时:清空本地 cursorArray,并给对端发 type=7
    // 请求双方清空光标索引、从 0 重新全量发送。带节流(见 cursorResyncRequested)。
    void requestCursorResync();

    // 编/解码状态 handle:codec(H264/H265/AV1/VP8...)+ 是否硬解
    // 由解码工厂在 Configure 后触发,MainWindow 据此更新主页 label
    std::function<void(const std::string& codec, bool hardDecode)> onCodecStatusHandle;

    void connect(std::string ip);

    void sendSignalingMessage(boost::json::object &  msg);

    void disConnect();

    void setOnVideoFrameHanlder(std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHanlder);

    std::string getAccountId() const;

    void setAccountId(const std::string& newAccountID);

    std::string getTargetId() const;

    // 被控端:由本机 System 经本地 TCP 控制通道(ENCODE_STATUS 消息)上报当前编码
    // codec + 硬编/软编,MainWindow 据此更新 label。
    std::function<void(const std::string& codec, bool hardEncode)> onEncodeStatusHandle;

    void setTargetId(const std::string& newTargetID);

    // 同步桌面配置(UI 改动后调用,使 manager 持有的 webrtcDeskConfig 始终最新;
    // RESTART/SYSTEMREADY 等内部 asyncRemoteDesk(webrtcDeskConfig) 复用路径会用上)
    void setWebrtcDeskConfig(const WebrtcDeskConfig& config);

    void writerRemote(unsigned char* data, size_t size);

    void asyncWrite(std::shared_ptr<WriterData> writerData);

    void webrtcAsyncWrite(std::string str);

    void disConnectRemote();

    std::function<void(void)> onSignalServerDisConnectHandle;

    std::function<void(void)> onFollowRemoteHandle;

    std::function<void(void)> onDisConnectRemoteHandle;

    std::function<void(void)> onRemoteSuccessFulHandle;

    std::function<void()> onSignalServerConnectHandle;

    std::function<void()> onRemoteFailedHandle;

    std::function<void()> onResetCursorHandle;

    std::function<void(int, double)> onRTCStatsCollectorHandle;

    // 主动请求一次 WebRTC 统计(触发 onRTCStatsCollectorHandle 回调,用于刷新 RTT)
    void requestStats();

    void disConnectHandle();

    void setSystemServiceExe(std::string webrtcExe);

    void handleCursor(const unsigned char* data,size_t size);

private:

    bool initializePeerConnection();

    boost::asio::awaitable<void> writerCoroutineAsync();

    void receiveCoroutineAysnc();

    void handleAsioException();

    void releaseSource();

    void disConnectRemoteHandler();

    void closeWebSocket();

    void setTcpKeepAlive(boost::asio::ip::tcp::socket & socket,
                         int idle = 0, int intvl =3, int probes = 3);

    boost::asio::awaitable<void> webrtcReceiveCoroutine();

    boost::asio::awaitable<void> webrtcWriteCoroutine();

public:

    std::atomic<bool> relativeMouseMode{false};

private:

    std::string accountId;

    std::string targetId;

    std::unique_ptr<webrtc::Thread> networkThread;

    std::unique_ptr<webrtc::Thread> workerThread;

    std::unique_ptr<webrtc::Thread> signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peerConnectionFactory;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;

    webrtc::scoped_refptr<webrtc::VideoTrackInterface> videoTrack;

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> videoSender;

    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audioTrack;

    webrtc::scoped_refptr<webrtc::RtpSenderInterface> audioSender;

    std::unique_ptr<PeerConnectionObserverImpl> peerConnectionObserver;

    std::unique_ptr<DataChannelObserverImpl> dataChannelObserver;

    webrtc::scoped_refptr<CreateOfferObserverImpl> createOfferObserver;

    webrtc::scoped_refptr<CreateAnswerObserverImpl> createAnswerObserver;

    std::unique_ptr<VideoTrackSinkImpl> videoSinkImpl;

    webrtc::scoped_refptr<RTCStatsCollectorHandle> rtcStatsCollectorHandle;

    WebrtcVideoEncoderFactory * webrtcVideoEncoderFactory;

    WebrtcVideoDecoderFactory* webrtcVideoDecoderFactory;

    std::atomic<bool> followRunning{false};

    std::atomic<int> videoWidth{1920};

    std::atomic<int> videoHeight{1080};

    std::atomic<bool> isRemote {false};

    boost::asio::io_context ioContext;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

    std::thread ioContextThread;

    // shared_ptr：connect/receive/write 协程各自持有一份引用，
    // 避免挂起期间成员被重连/关闭置空后对象被销毁导致野指针。
    std::shared_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> webSocket;

    boost::asio::ssl::context sslContext{ boost::asio::ssl::context::tlsv12_client };

    AsioConcurrentQueue<std::string> webrtcAsioConcurrentQueue;

    std::atomic<bool> webrtcAsyncEvents{ false };

    boost::asio::steady_timer reloadTimer;

    boost::asio::ip::tcp::acceptor accept;

    std::atomic<bool> asyncAccpets {false};

    std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHandler;

    std::shared_ptr<boost::asio::ip::tcp::socket> tcpSocket;

    AsioConcurrentQueue<std::shared_ptr<WriterData>> asioConcurrentQueue;

    std::atomic<bool> asyncEvents{false};

    std::string followData;

    std::string systemService = "HopeDeskSystem";

    std::string systemServiceExe;

    std::vector<std::vector<unsigned char>> cursorArray ;

    std::atomic<bool> cursorCacheDirty{false};

    std::atomic<bool> cursorResyncRequested{false};


    static constexpr std::chrono::seconds TIME_OUT{5};

    WebrtcDeskConfig webrtcDeskConfig;

};


}

}

#endif // WebRTCManager_H
