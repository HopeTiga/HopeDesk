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

#include "factory/WebrtcVideoEncoderFactory.h"
#include "factory/WebrtcVideoDecoderFactory.h"
#include "impl/PeerConnectionObserverImpl.h"
#include <d3d11.h>
#include "codec/D3D11VideoFrameData.h"
#include "impl/VideoTrackSinkImpl.h"
#include "audio/AudioDeviceModuleImpl.h"
#include "impl/DataChannelObserverImpl.h"
#include "impl/SetDescriptionObserverImpl.h"
#include "impl/CreateDescriptionObserverImpl.h"
#include "impl/RTCStatsCollectorHandle.h"

#include <boost/asio.hpp>

#include <boost/json.hpp>

#include "../system/WindowsServiceManager.h"
#include "../utils/Utils.h"
#include "../net/WebSocket.h"
#include "../net/TcpSocket.h"
#include "../net/TcpAcceptor.h"
#include "../net/Socket.h"


#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

// 前向声明 D3D11 接口，避免在本头里拉入 <d3d11.h>
struct ID3D11Texture2D;

namespace hope{

namespace rtc{

using hope::net::WebSocket;

using hope::net::TcpSocket;

using hope::net::TcpAcceptor;

using hope::net::WriterData;

using hope::net::WebrtcEnvelope;

using hope::system::WindowsServiceManager;

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

// 本端在连接里的角色:
//  - Caller  主动方/调用方/操控端:发起 REQUEST
//  - Callee  被动方/被调用方/被控端:收到 REQUEST
enum class WebrtcRole {
    None,
    Caller,
    Callee
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

    bool webrtcAsyncWrite(std::string str);

    void post(std::function<void()> task);

    void cancelRequestTimeout();

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

    void releaseSource();

    void closeTcpSocket();

    void disConnectRemoteHandler();

    // 请求超时看门狗:主动方 15s 失败上报 UI;被动方 30s 静默清理。一次只存活一个。
    void armRequestTimeout(WebrtcRole role);

    void handleSignalMessage(std::string str);

    void handleSignalServerDisconnect();

    void handleSystemAccept();

    void handleSystemMessage(std::string str);

    void handleSystemDisconnect();

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

    WebrtcVideoEncoderFactory * webrtcVideoEncoderFactory = nullptr;

    WebrtcVideoDecoderFactory * webrtcVideoDecoderFactory = nullptr;

    std::atomic<bool> isRemote {false};

    boost::asio::io_context ioContext;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

    std::thread ioContextThread;

    std::shared_ptr<WebSocket> webSocket;

    std::shared_ptr<TcpAcceptor> tcpAcceptor;

    std::shared_ptr<TcpSocket> tcpSocket;

    std::shared_ptr<boost::asio::steady_timer> requestTimeout;

    uint64_t timeoutEpoch = 0;

    std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHandler;

    std::string followData;

    std::vector<std::vector<unsigned char>> cursorArray ;

    std::atomic<bool> cursorCacheDirty{false};

    std::atomic<bool> cursorResyncRequested{false};

    WebrtcDeskConfig webrtcDeskConfig;

    WebrtcManagerConfig webrtcManagerConfig;

};

}

}

#endif // WebRTCManager_H
