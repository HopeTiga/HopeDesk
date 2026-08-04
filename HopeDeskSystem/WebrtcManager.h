#pragma once
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

#include <api/media_stream_interface.h>
#include <api/rtp_receiver_interface.h>
#include <api/rtp_transceiver_interface.h>
#include <api/jsep.h>
#include <api/rtc_error.h>
#include <api/scoped_refptr.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <rtc_base/thread.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/ref_counted_object.h>
#include <pc/video_track_source.h>
#include <api/enable_media_with_defaults.h>
#include <media/base/adapted_video_track_source.h>
#include <common_video/include/video_frame_buffer_pool.h> 

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json.hpp>

#include "WebrtcVideoEncoderFactory.h"
#include "WebrtcVideoDecoderFactory.h"
#include "PeerConnectionObserverImpl.h"
#include "VideoTrackSourceImpl.h"
#include "AudioDeviceModuleImpl.h"
#include "DataChannelObserverImpl.h"
#include "SetDescriptionObserverImpl.h"
#include "CreateDescriptionObserverImpl.h"
#include "RTCStatsCollectorHandle.h"

// Project includes
#include "AsioConcurrentQueue.h"
#include "VirtualDisplayCapture.h"
#include "ScreenCapture.h"
#include "HAudioCatch.h"
#include "WinLogon.h"
#include "KeyMouseSimulator.h"
#include "CursorHooks.h"
#include "Utils.h"

#include "HWebRTC.h"

namespace hope {

    namespace rtc {

        struct WebrtcDeskSystemConfig {
            int webrtcModulesType = 0;        // 0=游戏模式 1=办公模式
            int webrtcUseLevels = 0;          // 采集层级/加速策略
            WebrtcVideoCodec videoCodec = WebrtcVideoCodec::AV1;  // 视频编码
            int webrtcAudioEnable = 0;        // 是否传音频
            int webrtcEnableNvenc = 0;         // 硬件编码(NVENC)
            int requestMaxBitrateBps = 15000000;  // 最大码率,默认 15 Mbps
            int requestMinBitrateBps = 15000000;  // 最小码率,默认 15 Mbps
            int requestMaxFramerate  = 144;        // 最大帧率
            int localMaxBitrateBps = 15000000;  // 最大码率,默认 15 Mbps
            int localMinBitrateBps = 15000000;  // 最小码率,默认 15 Mbps
            int localMaxFramerate = 144;        // 最大帧率
            int desktopWidth  = 1920;           // 虚拟显示器宽度(客户端传入)
            int desktopHeight = 1080;           // 虚拟显示器高度(客户端传入)
            int desktopRefreshRate = 144;       // 虚拟显示器刷新率Hz(客户端传入)
        };

        // WebrtcManager 运行期配置:STUN/TURN 由 Native 端通过 REGISTER 消息(registerStr)
        // 直接传入,WebrtcManager 自身不再读取 ConfigManager,便于后续扩展更多字段。
        struct WebrtcManagerConfig {
            std::string stunHost;
            std::string turnHost;
            std::string turnUsername;
            std::string turnPassword;
            std::string systemService; // stable id; ties a virtual display to this service
        };

        class WebrtcManager {

            friend class PeerConnectionObserverImpl;

            friend class DataChannelObserverImpl;

        public:

            WebrtcManager(std::function<void()> closeHandle, WebrtcDeskSystemConfig config = WebrtcDeskSystemConfig{});

            ~WebrtcManager();

            void Cleanup();

            void sendSignalingMessage(const boost::json::object& message);

            void processOffer(const std::string& sdp);

            void processAnswer(const std::string& sdp);

            void processIceCandidate(const std::string& candidate, const std::string& mid, int lineIndex);

            void asyncWrite(std::shared_ptr<WriterData> data);

            void releaseSource();

        private:

            void socketEventLoop();

            bool initializePeerConnection();

            bool initializeScreenCapture();

            bool initializeHAudioCatch();

            void handleDataChannelData(const unsigned char* data, size_t size);

            static webrtc::RtpEncodingParameters getDefaultRtpEncodingParameters() {
                webrtc::RtpEncodingParameters encoding;
                encoding.active = true;
                encoding.max_bitrate_bps = 15000000;  // 4 Mbps
                encoding.min_bitrate_bps = 15000000;  // 1 Mbps
                encoding.bitrate_priority = 4.0;
                encoding.max_framerate = 144;
                encoding.scale_resolution_down_by = 1.0;
                encoding.bitrate_priority = 1.0;
                encoding.scalability_mode = "L1T1";
                encoding.network_priority = webrtc::Priority::kHigh;
                return encoding;
            }

            boost::asio::awaitable<void> receiveCoroutine();

            boost::asio::awaitable<void> writerCoroutine();

        private:

            std::string accountId;

            std::string targetId;

            WebrtcDeskSystemConfig webrtcDeskSystemConfig;

            WebrtcManagerConfig webrtcManagerConfig;

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

            webrtc::scoped_refptr<VideoTrackSourceImpl> videoTrackSourceImpl;

            webrtc::scoped_refptr<AudioDeviceModuleImpl> audioDeviceModuleImpl;

            webrtc::scoped_refptr<RTCStatsCollectorHandle> rtcStatsCollectorHandle;

            webrtc::VideoFrameBufferPool bufferPool;

            WebrtcVideoEncoderFactory* webrtcVideoEncoderFactory;

            WebrtcVideoDecoderFactory* webrtcVideoDecoderFactory;

            std::shared_ptr<VirtualDisplayCapture> screenCapture;  // 高性能模式：VDD 帧通道
            std::shared_ptr<ScreenCapture> compatScreenCapture;    // 兼容模式：DXGI 桌面采集
            std::string activeCaptureTech = "Hope Virtual Display";  // 上报用: "Hope Virtual Display" 或 "Desktop Duplication API"

            std::shared_ptr<HAudioCatch> hAudioCatch;

            std::atomic<bool> socketRuns{ false };

            boost::asio::io_context ioContext;

            std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

            std::thread ioContextThread;

            std::unique_ptr<boost::asio::ip::tcp::socket> tcpSocket;

            AsioConcurrentQueue<std::shared_ptr<WriterData>> asioConcurrentQueue;

            std::unique_ptr<WinLogon> winLogon;

            std::unique_ptr<KeyMouseSimulator> keyMouseSim;

            std::unique_ptr<CursorHooks> cursorHooks;

            std::function<void()> closeHandle;

        };

    }

}