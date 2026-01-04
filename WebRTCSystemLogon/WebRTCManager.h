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
#include <third_party/libyuv/include/libyuv.h>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/json.hpp>

#include "PeerConnectionObserverImpl.h"
#include "VideoTrackSourceImpl.h"
#include "AudioDeviceModuleImpl.h"
#include "DataChannelObserverImpl.h"
#include "SetDescriptionObserverImpl.h"
#include "CreateDescriptionObserverImpl.h"

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

            bool initializeScreenCapture(int webrtcModulesType = 0,int webrtcUseGPU = 0);

            void handleDataChannelData(const unsigned char* data, size_t size);

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

            std::string accountId;

            std::string targetId;

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

            webrtc::VideoFrameBufferPool bufferPool;

            std::atomic<bool> isInit{ false };

            std::atomic<WebRTCRemoteState> state;

            std::atomic<WebRTCConnetState> connetState;

            std::shared_ptr<ScreenCapture> screenCapture;

            std::atomic<bool> socketRuns{ false };

            boost::asio::io_context ioContext;

            std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr;

            std::thread ioContextThread;

            std::unique_ptr<boost::asio::ip::tcp::socket> tcpSocket;

            boost::asio::experimental::channel<void(boost::system::error_code)> writerChannel;

            moodycamel::ConcurrentQueue<std::shared_ptr<WriterData>> writerDataQueues{ 1 };

            std::unique_ptr<WinLogon> winLogon;

            std::unique_ptr<KeyMouseSimulator> keyMouseSim;

            std::unique_ptr<CursorHooks> cursorHooks;

        };

    }

}