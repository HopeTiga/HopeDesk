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

#include "WebRTCVideoEncoderFactory.h"
#include "WebRTCVideoDecoderFactory.h"
#include "PeerConnectionObserverImpl.h"
#include "VideoTrackSourceImpl.h"
#include "AudioDeviceModuleImpl.h"
#include "DataChannelObserverImpl.h"
#include "SetDescriptionObserverImpl.h"
#include "CreateDescriptionObserverImpl.h"
#include "RTCStatsCollectorHandle.h"

// Project includes
#include "AsioConcurrentQueue.h"
#include "ScreenCapture.h"
#include "HAudioCatch.h"
#include "WinLogon.h"
#include "KeyMouseSimulator.h"
#include "CursorHooks.h"
#include "Utils.h"

#include "HWebRTC.h"

namespace hope {

    namespace rtc {

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

            void asyncWrite(std::shared_ptr<WriterData> data);

            void releaseSource();

        private:

            void socketEventLoop();

            bool initializePeerConnection();

            bool initializeScreenCapture(int webrtcModulesType = 0, int webrtcUseLevels = 0);

            bool initializeHAudioCatch();

            void handleDataChannelData(const unsigned char* data, size_t size);

            static webrtc::RtpEncodingParameters getDefaultRtpEncodingParameters() {
                webrtc::RtpEncodingParameters encoding;
                encoding.active = true;
                encoding.max_bitrate_bps = 8000000;  // 4 Mbps
                encoding.min_bitrate_bps = 2000000;  // 1 Mbps
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

            webrtc::scoped_refptr<webrtc::RtpSenderInterface> audioSender;

            std::unique_ptr<PeerConnectionObserverImpl> peerConnectionObserver;

            std::unique_ptr<DataChannelObserverImpl> dataChannelObserver;

            webrtc::scoped_refptr<CreateOfferObserverImpl> createOfferObserver;

            webrtc::scoped_refptr<CreateAnswerObserverImpl> createAnswerObserver;

            webrtc::scoped_refptr<VideoTrackSourceImpl> videoTrackSourceImpl;

            webrtc::scoped_refptr<AudioDeviceModuleImpl> audioDeviceModuleImpl;

            webrtc::scoped_refptr<RTCStatsCollectorHandle> rtcStatsCollectorHandle;

            webrtc::VideoFrameBufferPool bufferPool;

            WebRTCVideoEncoderFactory* webrtcVideoEncoderFactory;

            WebRTCVideoDecoderFactory* webrtcVideoDecoderFactory;

            std::atomic<WebRTCConnetState> connetState;

            std::shared_ptr<ScreenCapture> screenCapture;

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

            int webrtcAudioEnable = 0;

            int webrtcEnableNvidia = 0;

        };

    }

}