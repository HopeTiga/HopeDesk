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
#include <d3d11.h>
#include "D3D11VideoFrameData.h"
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
    STOPREMOTE = 3,
    START = 4,
    CLOSESYSTEM = 6,
    SYSTEMREADLY = 7,
    STATS = 8,
    ENCODE_STATUS = 9   // System -> 被控 Native:上报当前编码 codec + 硬编/软编
};


enum class FrameFormat {
    Unknown,
    I420,
    Nv12,
    Nv12Gpu   // D3D11 解码产出的 NV12 共享纹理(已打开到渲染设备),零拷贝,不拷 CPU
};

struct VideoFrame {
    FrameFormat format = FrameFormat::I420;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer;

    webrtc::scoped_refptr<const webrtc::NV12BufferInterface> nv12Buffer;

    // Nv12Gpu:解码器槽位池产出的帧数据(NV12 纹理 + plane SRV + 忙标志)。
    // 渲染端画完(经 frameToReleasePtr 延迟一帧)后把 busy 置 false 释放回池。
    std::shared_ptr<D3D11VideoFrameData> d3d11FrameData;

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

// WebrtcManager 启动期配置(由 MainWindow 从 config.ini 读取后注入,WebrtcManager 自身不再读配置)
struct WebrtcManagerConfig {
    std::string systemService = "HopeDeskSystem";   // 系统服务名(Webrtc.SystemService,用户可设)
    std::string systemServiceExe;                   // 系统服务可执行文件路径(Webrtc.SystemServiceExe)
    std::string stunHost;                            // STUN 服务器 URI(Stun.Host)
    std::string turnHost;                            // TURN 服务器 URI(Turn.Host)
    std::string turnUsername;                        // TURN 用户名(Turn.Username)
    std::string turnPassword;                        // TURN 密码(Turn.Password)
    bool webrtcDebugLog = false;                     // WebRTC 调试日志(Webrtc.DebugLog),随 registerJson 传给 System
};

struct WebrtcDeskConfig {

    int webrtcModulesType = 0;   // 0=游戏模式 1=办公模式
    int webrtcUseLevels = 2;     // 加速策略/采集层级
    int videoCodec = 4;          // 视频编码索引
    int webrtcAudioEnable = 0;   // 是否传音频
    int webrtcEnableNvenc = 0;    // 硬件编码(NVENC),发给 System
    int webrtcEnableD3D11 = 0;   // 硬件解码(MF/D3D11),Native 本地

    int requestMaxBitrateBps = 15000000;  // 请求组最大码率,默认 15 Mbps
    int requestMinBitrateBps = 15000000;  // 请求组最小码率,默认 15 Mbps
    int requestMaxFramerate  = 144;        // 请求组最大帧率
    int localMaxBitrateBps   = 15000000;  // 本地组最大码率,默认 15 Mbps
    int localMinBitrateBps   = 15000000;  // 本地组最小码率,默认 15 Mbps
    int localMaxFramerate    = 144;       // 本地组最大帧率

    // Hope Vitrual Display 虚拟显示器配置(独立于 WebRTC 帧率)
    int desktopWidth       = 1920;
    int desktopHeight      = 1080;
    int desktopRefreshRate = 144;

};


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

    // 注入启动期配置(服务/可执行路径/配置路径/STUN/TURN),由 MainWindow 读取后注入
    void setWebrtcManagerConfig(const WebrtcManagerConfig& webrtcManagerConfig);

    // 运行期改 WebRTC 调试日志开关:更新配置并立即重发 REGISTER 推给本地 System(若 TCP 已连)
    void applyWebrtcDebugLog(bool enabled);

    void sendKeyComboCtrlAltF();

    void resetCursorCache();

    void requestCursorResync();

    std::function<void(const std::string& codec, bool hardDecode)> onCodecStatusHandle;

    void connect(std::string ip);

    void sendSignalingMessage(boost::json::object &  msg);

    void disConnect();

    void setOnVideoFrameHanlder(std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHanlder);

    std::string getAccountId() const;

    void setAccountId(const std::string& newAccountID);
    // 注入渲染端(QRhi)的 D3D11 设备,透传给解码器做零拷贝(解码器与渲染器共用同设备)。
    void setDecoderD3D11Device(ID3D11Device* dev);

    std::string getTargetId() const;

    void setTargetId(const std::string& newTargetID);

    void setWebrtcDeskConfig(const WebrtcDeskConfig& config);

    void abortPendingConnection();

    void writerRemote(unsigned char* data, size_t size);

    void asyncWrite(std::shared_ptr<WriterData> writerData);

    void webrtcAsyncWrite(std::string str);

    // 把任务投递到 ioContext 线程执行,保证与 ioContext 上的操作(收发/定时器)串行、线程安全。
    // peerConnection 等回调运行在 WebRTC 信令线程,必须经此投递后再碰 WebrtcManager 状态。
    void post(std::function<void()> task);

    void disConnectRemote();

    std::function<void(void)> onSignalServerDisConnectHandle;

    std::function<void(void)> onFollowRemoteHandle;

    std::function<void(void)> onDisConnectRemoteHandle;

    std::function<void(void)> onRemoteSuccessFulHandle;

    std::function<void()> onSignalServerConnectHandle;

    std::function<void()> onRemoteFailedHandle;

    std::function<void()> onResetCursorHandle;

    std::function<void(int, double)> onRTCStatsCollectorHandle;

    std::function<void(const std::string& codec, bool hardEncode, const std::string& captureTech)> onEncodeStatusHandle;

    void requestStats();

    void disConnectHandle();

    void handleCursor(const unsigned char* data,size_t size);

private:

    bool initializePeerConnection();

    boost::asio::awaitable<void> writerCoroutineAsync();

    void receiveCoroutineAysnc();

    void releaseSource();

    void closeTcpSocket();

    void disConnectRemoteHandler();

    void closeWebSocket();

    void setTcpKeepAlive(boost::asio::ip::tcp::socket & socket,
                         int idle = 0, int intvl =3, int probes = 3);

    // 唯一看门狗:发请求时 arm,15 秒内再发请求则刷新(取消旧 timer),只有最后一次请求
    // 发出后 15s 仍没连上才触发一次 re-init。避免每次请求各建一个 timer 导致的并发 re-init。
    void armRequestTimeout();

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

    std::shared_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> webSocket;

    boost::asio::ssl::context sslContext{ boost::asio::ssl::context::tlsv12_client };

    AsioConcurrentQueue<std::string> webrtcAsioConcurrentQueue;

    std::atomic<bool> webrtcAsyncEvents{ false };

    // 请求超时看门狗。一次只存活一个;新请求先 cancel 旧 timer 再重建。
    // timeoutEpoch 递增,旧协程醒来后按 epoch 不匹配直接退出,双保险防并发 re-init。
    std::shared_ptr<boost::asio::steady_timer> requestTimeout;

    uint64_t timeoutEpoch = 0;

    boost::asio::ip::tcp::acceptor accept;

    std::atomic<bool> asyncAccpets {false};

    std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHandler;

    std::shared_ptr<boost::asio::ip::tcp::socket> tcpSocket;

    AsioConcurrentQueue<std::shared_ptr<WriterData>> asioConcurrentQueue;

    std::atomic<bool> asyncEvents{false};

    std::string followData;

    std::vector<std::vector<unsigned char>> cursorArray ;

    std::atomic<bool> cursorCacheDirty{false};

    std::atomic<bool> cursorResyncRequested{false};

    static constexpr std::chrono::seconds TIME_OUT{5};

    WebrtcDeskConfig webrtcDeskConfig;

    WebrtcManagerConfig webrtcManagerConfig;

};


}

}

#endif // WebRTCManager_H
