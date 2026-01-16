#include "WebRTCManager.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/video/i420_buffer.h>
#include <api/field_trials.h>
#include "WebRTCVideoEncoderFactory.h"
#include "ConfigManager.h"


namespace hope {

    namespace rtc {


        WebRTCManager::WebRTCManager(WebRTCVideoCodec codec, webrtc::RtpEncodingParameters rtpEncodingParameters)
            : tcpSocket(std::make_unique<boost::asio::ip::tcp::socket>(ioContext)),
            state(WebRTCRemoteState::nullRemote),
            connetState(WebRTCConnetState::none),
            peerConnection(nullptr),
            writerChannel(ioContext),
            winLogon(nullptr),
            keyMouseSim(nullptr),
            codec(codec),
            rtpEncodingParameters(rtpEncodingParameters),
            bufferPool(false, 100),
            cursorHooks(nullptr),
            screenCapture(nullptr),
            hAudioCatch(nullptr) {

            ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
                boost::asio::make_work_guard(ioContext));

            ioContextThread = std::move(std::thread([this]() {
                this->ioContext.run();
                }));

            boost::asio::co_spawn(ioContext, [this]()-> boost::asio::awaitable<void> {

                tcpSocket = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);

                boost::system::error_code ec;

                auto address = boost::asio::ip::make_address("127.0.0.1", ec);
                if (ec) {
                    LOG_ERROR("Failed to parse address 127.0.0.1: %s", ec.message());
                    co_return;
                }

                boost::asio::ip::tcp::endpoint endpoint(address, 19998);

                // 使用带超时的连接（可选）
                co_await boost::asio::async_connect(
                    *tcpSocket,
                    std::array{ endpoint },
                    boost::asio::use_awaitable
                );

                LOG_INFO("TCP connection accepted");

                socketEventLoop();

                }, [this](std::exception_ptr p) {
                    try {
                        if (p) {
                            std::rethrow_exception(p);
                        }
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("TCP acceptor error: %s", e.what());
                    }
                    });

                keyMouseSim = std::make_unique<KeyMouseSimulator>();

                if (!keyMouseSim->Initialize()) {
                    LOG_ERROR("KeyMouseSimulator initialization failed");
                }

        }

        void WebRTCManager::sendSignalingMessage(const boost::json::object& message) {
            boost::json::object fullMsg;
            fullMsg["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
            fullMsg["accountId"] = accountId;
            fullMsg["targetId"] = targetId;
            fullMsg["state"] = 200;

            for (auto& [key, value] : message) {
                fullMsg[key] = value;
            }

            std::string msgStr = boost::json::serialize(fullMsg);
            auto data = std::make_shared<WriterData>(const_cast<char*>(msgStr.c_str()), msgStr.size());

            writerAsync(data);
        }

        void WebRTCManager::processOffer(const std::string& sdp) {
            if (sdp.empty()) {
                LOG_ERROR("Received empty SDP offer");
                isInit = false;
                return;
            }

            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
                webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

            if (desc) {
                peerConnection->SetRemoteDescription(
                    SetRemoteDescriptionObserver::Create().get(), desc.release());

                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                createAnswerObserver = CreateAnswerObserverImpl::Create(this, peerConnection);
                peerConnection->CreateAnswer(createAnswerObserver.get(), options);
            }
            else {
                LOG_ERROR("Failed to parse offer: %s", error.description);
                isInit = false;
            }
        }

        void WebRTCManager::processAnswer(const std::string& sdp) {
            if (sdp.empty()) {
                LOG_ERROR("Received empty SDP answer");
                isInit = false;
                return;
            }

            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
                webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);

            if (desc) {
                peerConnection->SetRemoteDescription(
                    SetRemoteDescriptionObserver::Create().get(), desc.release());
            }
            else {
                LOG_ERROR("Failed to parse answer: %s", error.description.c_str());
                isInit = false;
            }
        }

        void WebRTCManager::processIceCandidate(const std::string& candidate,
            const std::string& mid, int lineIndex) {
            if (candidate.empty()) {
                return;
            }

            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::IceCandidateInterface> iceCandidate(
                webrtc::CreateIceCandidate(mid, lineIndex, candidate, &error));

            if (iceCandidate) {
                peerConnection->AddIceCandidate(iceCandidate.release());
            }
            else {
                LOG_ERROR("Failed to parse ICE candidate: %s", error.description.c_str());
            }
        }

        void WebRTCManager::socketEventLoop() {
            socketRuns = true;

            // 读取协程
            boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
                try {
                    char headerBuffer[8];
                    size_t headerSize = sizeof(int64_t);

                    while (socketRuns) {
                        std::memset(headerBuffer, 0, headerSize);

                        // 接收消息头
                        size_t headerRead = 0;
                        while (headerRead < headerSize) {
                            try {
                                size_t n = co_await this->tcpSocket->async_read_some(
                                    boost::asio::buffer(headerBuffer + headerRead, headerSize - headerRead),
                                    boost::asio::use_awaitable);

                                if (n == 0) {
                                    co_return;
                                }

                                headerRead += n;
                            }
                            catch (const boost::system::system_error& e) {
                                LOG_ERROR("Socket read error: %s", e.what());
                                co_return;
                            }
                        }

                        int64_t rawBodyLength = 0;
                        fastCopy(&rawBodyLength, headerBuffer, sizeof(int64_t));
                        int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

                        if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) {
                            LOG_ERROR("Invalid body length: %d", bodyLength);
                            co_return;
                        }

                        size_t bodySize = static_cast<size_t>(bodyLength);

                        std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]);
                        if (!bodyBuffer) {
                            LOG_ERROR("Failed to allocate buffer");
                            co_return;
                        }
                        std::memset(bodyBuffer.get(), 0, bodySize + 1);

                        // 接收消息体
                        size_t bodyRead = 0;
                        while (bodyRead < bodySize) {
                            try {
                                size_t n = co_await this->tcpSocket->async_read_some(
                                    boost::asio::buffer(bodyBuffer.get() + bodyRead, bodySize - bodyRead),
                                    boost::asio::use_awaitable);

                                if (n == 0) {
                                    co_return;
                                }

                                bodyRead += n;
                            }
                            catch (const boost::system::system_error& e) {
                                LOG_ERROR("Socket read error: %s", e.what());
                                co_return;
                            }
                        }

                        std::string bodyStr(bodyBuffer.get(), bodySize);

                        // 解析JSON
                        try {
                            boost::json::object json = boost::json::parse(bodyStr).as_object();

                            if (json.contains("requestType")) {
                                int64_t requestType = json["requestType"].as_int64();

                                if (!json.contains("state")) {
                                    continue;
                                }

                                int64_t responseState = json["state"].as_int64();

                                if (WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER)
                                {

                                    ConfigManager::Instance().Load(json["webrtcManagerPath"].as_string().c_str() + std::string("/config.ini"));

                                }

                                else if (WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST) {
                                    if (responseState == 200) {
                                        if (json.contains("webRTCRemoteState")) {
                                            WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());

                                            if (state.load() == WebRTCRemoteState::nullRemote) {

                                                if (remoteState == WebRTCRemoteState::masterRemote) {

                                                    state = WebRTCRemoteState::followerRemote;


                                                    if (json.contains("codec")) {
                                                        codec = static_cast<WebRTCVideoCodec>(json["codec"].as_int64());
                                                    }

                                                    if (json.contains("webrtcAudioEnable")) {

                                                        webrtcAudioEnable = json["webrtcAudioEnable"].as_int64();

                                                    }

                                                    if (!initializePeerConnection()) {
                                                        LOG_ERROR("Failed to initialize peer connection");
                                                        continue;
                                                    }

                                                    int webrtcModulesType = 0;

                                                    int webrtcUseGPU = 0;

                                                    if (json.contains("webrtcModulesType")) {

                                                        webrtcModulesType = json["webrtcModulesType"].as_int64();

                                                    }

                                                    if (json.contains("webrtcUseGPU")) {

                                                        webrtcUseGPU = json["webrtcUseGPU"].as_int64();

                                                    }

                                                    if (!initializeScreenCapture(webrtcModulesType, webrtcUseGPU)) {
                                                        LOG_ERROR("Failed to initialize ScreenCapture");
                                                        continue;
                                                    }

                                                    if (webrtcAudioEnable == 1) {
                                                        if (!initializeHAudioCatch()) {
                                                            LOG_ERROR("Failed to initialize HAudioCatch");
                                                            continue;

                                                        }
                                                    }

                                                    if (json.contains("accountId")) {
                                                        targetId = std::string(json["accountId"].as_string().c_str());
                                                    }

                                                    if (json.contains("targetId")) {
                                                        accountId = std::string(json["targetId"].as_string().c_str());
                                                    }

                                                    if (!isProcessingOffer.exchange(true)) {
                                                        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                                                        options.offer_to_receive_video = true;
                                                        options.offer_to_receive_audio = false;

                                                        createOfferObserver = CreateOfferObserverImpl::Create(this, peerConnection);
                                                        peerConnection->CreateOffer(createOfferObserver.get(), options);
                                                    }
                                                }
                                            }
                                        }

                                        if (json.contains("type")) {
                                            std::string type(json["type"].as_string().c_str());

                                            if (type == "answer") {
                                                if (!isInit.load()) {
                                                    std::string sdp(json["sdp"].as_string().c_str());
                                                    processAnswer(sdp);

                                                    isInit = true;
                                                    isProcessingOffer = false;
                                                }
                                            }
                                            else if (type == "candidate") {
                                                std::string candidateStr(json["candidate"].as_string().c_str());
                                                std::string mid = json.contains("mid") ? std::string(json["mid"].as_string().c_str()) : "";
                                                int lineIndex = json.contains("mlineIndex") ? json["mlineIndex"].as_int64() : 0;

                                                if (peerConnection) {
                                                    processIceCandidate(candidateStr, mid, lineIndex);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("JSON parse error: %s", e.what());
                        }
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("Reader coroutine error: %s", e.what());
                }

                co_return;

                }, [this](std::exception_ptr p) {
                    try {
                        if (p) {
                            std::rethrow_exception(p);
                        }
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("Reader coroutine exception: %s", e.what());
                    }
                    });

                // 写入协程
                boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
                    try {
                        for (;;) {

                            std::shared_ptr<WriterData> nowNode = nullptr;

                            while (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {

                                try {
                                    co_await boost::asio::async_write(*this->tcpSocket,
                                        boost::asio::buffer(nowNode->data, nowNode->size),
                                        boost::asio::use_awaitable);

                                }
                                catch (const boost::system::system_error& e) {

                                    LOG_ERROR("Socket write error: %s", e.what());

                                    break;

                                }
                            }

                            if (!socketRuns) {

                                std::shared_ptr<WriterData> nowNode = nullptr;

                                while (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                                    try {
                                        co_await boost::asio::async_write(*this->tcpSocket,
                                            boost::asio::buffer(nowNode->data, nowNode->size),
                                            boost::asio::use_awaitable);
                                    }
                                    catch (const std::exception& e) {
                                        break;
                                    }
                                }

                                co_return;
                            }
                            else {

                                co_await this->writerChannel.async_receive(boost::asio::use_awaitable);

                            }

                        }
                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("Writer coroutine error: %s", e.what());

                    }

                    co_return;

                    }, [this](std::exception_ptr p) {
                        try {

                            if (p) {

                                std::rethrow_exception(p);

                            }

                        }
                        catch (const std::exception& e) {

                            LOG_ERROR("Writer coroutine exception: %s", e.what());

                        }
                        });
        }

        inline void WebRTCManager::writerAsync(std::shared_ptr<WriterData> data) {

            if (!data) {

                return;

            }

            writerDataQueues.enqueue(data);

            if (writerChannel.is_open()) {

                writerChannel.try_send(boost::system::error_code{});

            }
        }

        bool WebRTCManager::initializePeerConnection() {
            // Clean up any existing connection first
            if (peerConnection) {

                releaseSource();

            }

            webrtc::InitializeSSL();

            if (!peerConnectionFactory) {

                networkThread = webrtc::Thread::CreateWithSocketServer();

                if (!networkThread) {

                    LOG_ERROR("Failed to create network thread");

                    return false;

                }
                networkThread->SetName("network_thread", nullptr);

                if (!networkThread->Start()) {

                    LOG_ERROR("Failed to start network thread");

                    return false;

                }

                workerThread = webrtc::Thread::Create();

                if (!workerThread) {

                    LOG_ERROR("Failed to create worker thread");

                    return false;
                }
                workerThread->SetName("worker_thread", nullptr);

                if (!workerThread->Start()) {

                    LOG_ERROR("Failed to start worker thread");

                    return false;
                }

                signalingThread = webrtc::Thread::Create();

                if (!signalingThread) {

                    LOG_ERROR("Failed to create signaling thread");

                    return false;

                }
                signalingThread->SetName("signaling_thread", nullptr);

                if (!signalingThread->Start()) {

                    LOG_ERROR("Failed to start signaling thread");

                    return false;

                }

                networkThread->PostTask([this]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    });

                workerThread->PostTask([this]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    });

                audioDeviceModuleImpl = AudioDeviceModuleImpl::Create();

                const char* field_trials =
                    "WebRTC-Bwe-TrendlineEstimatorSettings/window_size:12,sort:true/"
                    "WebRTC-SendNackDelayMs/0/"
                    "WebRTC-Video-Pacing/factor:1.2,max_delay:200ms/"
                    "WebRTC-Pacer-DrainQueue/Enabled/"
                    "WebRTC-Pacer-PadInSilence/Enabled/"
                    "WebRTC-Bwe-ProbingConfiguration/Enabled/"
                    "WebRTC-LossBasedBweV2/Enabled/";

                std::unique_ptr<webrtc::FieldTrialsView> fieldTrials = std::make_unique<webrtc::FieldTrials>(field_trials);

                peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
                    networkThread.get(),
                    workerThread.get(),
                    signalingThread.get(),
                    audioDeviceModuleImpl,
                    webrtc::CreateBuiltinAudioEncoderFactory(),
                    webrtc::CreateBuiltinAudioDecoderFactory(),
                    std::make_unique<WebRTCVideoEncoderFactory>(),
                    webrtc::CreateBuiltinVideoDecoderFactory(),
                    nullptr,
                    nullptr,
                    nullptr,
                    std::move(fieldTrials)
                );

                if (!peerConnectionFactory) {

                    LOG_ERROR("Failed to create PeerConnectionFactory");

                    return false;

                }
            }

            webrtc::PeerConnectionInterface::RTCConfiguration config;

            config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

            config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;

            config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

            config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;

            config.ice_connection_receiving_timeout = 10000;        // 5秒无数据包则认为断开

            config.ice_unwritable_timeout = 10000;                  // 3秒无响应则标记为不可写

            config.ice_inactive_timeout = 10000;                    // 5秒后标记为非活跃

            config.set_dscp(true);

            webrtc::PeerConnectionInterface::IceServer stunServer;

            stunServer.uri = ConfigManager::Instance().GetString("Stun.Host");

            config.servers.push_back(stunServer);

            webrtc::PeerConnectionInterface::IceServer turnServer;

            turnServer.uri = ConfigManager::Instance().GetString("Turn.Host");

            turnServer.username = ConfigManager::Instance().GetString("Turn.Username");

            turnServer.password = ConfigManager::Instance().GetString("Turn.Password");

            config.servers.emplace_back(turnServer);

            peerConnectionObserver = std::make_unique<PeerConnectionObserverImpl>(this);

            webrtc::PeerConnectionDependencies pcDependencies(peerConnectionObserver.get());

            auto pcResult = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pcDependencies));

            if (!pcResult.ok()) {

                LOG_ERROR("Failed to create PeerConnection: %s", pcResult.error().message());

                return false;

            }

            peerConnection = pcResult.MoveValue();

            // 如果是发送端，创建视频源和轨道
            if (state == WebRTCRemoteState::followerRemote) {

                videoTrackSourceImpl = webrtc::make_ref_counted<VideoTrackSourceImpl>();

                videoTrack = peerConnectionFactory->CreateVideoTrack(videoTrackSourceImpl, "videoTrack");

                if (!videoTrack) {

                    LOG_ERROR("Failed to create video track");

                    return false;

                }

                videoTrack->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);

                std::vector<webrtc::RtpEncodingParameters> encodings;

                encodings.push_back(rtpEncodingParameters);

                std::vector<std::string> streamIds = { "mediaStream" };

                auto addTrackResult = peerConnection->AddTrack(videoTrack, streamIds, encodings);

                if (!addTrackResult.ok()) {

                    LOG_ERROR("Failed to add video track: %s", addTrackResult.error().message());

                    return false;

                }

                videoSender = addTrackResult.MoveValue();

                auto transceivers = peerConnection->GetTransceivers();

                for (auto& transceiver : transceivers) {

                    if (transceiver->media_type() == webrtc::MediaType::MEDIA_TYPE_VIDEO) {

                        webrtc::RtpCapabilities senderCapabilities = peerConnectionFactory->GetRtpSenderCapabilities(
                            webrtc::MediaType::MEDIA_TYPE_VIDEO);

                        senderCapabilities.fec.clear();

                        if (senderCapabilities.codecs.empty()) {

                            LOG_WARNING("No video codecs available from factory");

                            continue;

                        }

                        std::vector<webrtc::RtpCodecCapability> preferredCodecs;
                        // 根据枚举选择优先编解码器
                        std::string priorityCodec;

                        switch (this->codec) {

                        case WebRTCVideoCodec::VP9: priorityCodec = "VP9"; break;

                        case WebRTCVideoCodec::H264: priorityCodec = "H264"; break;

                        case WebRTCVideoCodec::VP8: priorityCodec = "VP8"; break;

                        case WebRTCVideoCodec::H265: priorityCodec = "H265"; break;

                        case WebRTCVideoCodec::AV1: priorityCodec = "AV1"; break;

                        }

                        LOG_INFO("Attempting to prioritize codec: %s", priorityCodec.c_str());

                        // 首先添加优先编解码器
                        bool foundPriorityCodec = false;

                        for (const auto& codec : senderCapabilities.codecs) {

                            if (codec.name == priorityCodec) {

                                preferredCodecs.push_back(codec);

                                foundPriorityCodec = true;

                                LOG_INFO("Found and prioritized codec: %s", codec.name.c_str());

                                break;
                            }
                        }

                        if (!foundPriorityCodec) {

                            LOG_WARNING("Priority codec %s not found in available codecs", priorityCodec.c_str());

                        }

                        // 添加其他可用编解码器（排除重复项和辅助编解码器）
                        for (const auto& codec : senderCapabilities.codecs) {

                            if (codec.name != priorityCodec) {

                                preferredCodecs.push_back(codec);

                                LOG_INFO("Added additional codec: %s", codec.name.c_str());

                            }
                        }

                        // 验证是否有编解码器可设置
                        if (preferredCodecs.empty()) {

                            LOG_ERROR("No valid codecs to set as preferences");

                            continue;

                        }

                        // 设置编解码器偏好
                        auto result = transceiver->SetCodecPreferences(preferredCodecs);

                        if (result.ok()) {

                            LOG_INFO("Successfully set codec preferences with %d codecs", preferredCodecs.size());

                        }
                        else {

                            LOG_ERROR("Failed to set codec preferences: %s", result.message());

                        }
                    }
                }

                webrtc::RtpParameters parameters = videoSender->GetParameters();

                if (!parameters.encodings.empty()) {

                    parameters.encodings[0] = rtpEncodingParameters;

                }
                else {
                    parameters.encodings = encodings;
                }

                parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;

                auto setParamsResult = videoSender->SetParameters(parameters);

                if (!setParamsResult.ok()) {
                    LOG_ERROR("Failed to set RTP parameters: %s", setParamsResult.message());
                    return false;
                }
            }

            if (webrtcAudioEnable == 1) {

                audioTrack = peerConnectionFactory->CreateAudioTrack("audioTrack", nullptr);

                std::vector<std::string> audioStreamIds = { "audioStream" };

                auto audioTrackResult = peerConnection->AddTrack(audioTrack, audioStreamIds);

                if (!audioTrackResult.ok()) {
                    LOG_ERROR("Failed to add video track: %s", audioTrackResult.error().message());
                    return false;
                }

                audioSender = audioTrackResult.MoveValue();

            }

            std::unique_ptr<webrtc::DataChannelInit> dataChannelConfig = std::make_unique<webrtc::DataChannelInit>();

            dataChannelConfig->priority = webrtc::PriorityValue(webrtc::Priority::kHigh);

            dataChannel = peerConnection->CreateDataChannel("dataChannel", dataChannelConfig.get());
            if (!dataChannel) {
                LOG_ERROR("Failed to create data channel");
                return false;
            }

            dataChannelObserver = std::make_unique<DataChannelObserverImpl>(this);

            dataChannel->RegisterObserver(dataChannelObserver.get());

            return true;
        }

        bool WebRTCManager::initializeScreenCapture(int webrtcModulesType, int webrtcUseGPU) {
            if (screenCapture) {
                return false;
            }

            screenCapture = std::make_shared<ScreenCapture>();

            hope::rtc::ScreenCapture::CaptureConfig config;

            if (webrtcModulesType == 0) {

                config.enableDirtyRects = false;

            }
            else if (webrtcModulesType == 1) {

                config.enableDirtyRects = true;

            }

            if (webrtcUseGPU == 0) {

                config.enableGPUYUV = true;

            }
            else if (webrtcUseGPU == 1) {

                config.enableGPUYUV = false;

            }

            screenCapture->setConfig(config);

            screenCapture->setDataHandle([this](const uint8_t* data, int stride, int width, int height, bool isYUV) {

                if (!videoTrackSourceImpl || !data) return;

                webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer =
                    bufferPool.CreateI420Buffer(width, height);

                if (!i420Buffer) return;

                uint8_t* dstY = i420Buffer->MutableDataY();
                int dstStrideY = i420Buffer->StrideY();
                uint8_t* dstU = i420Buffer->MutableDataU();
                int dstStrideU = i420Buffer->StrideU();
                uint8_t* dstV = i420Buffer->MutableDataV();
                int dstStrideV = i420Buffer->StrideV();

                if (isYUV) {

                    const int ySize = width * height;
                    const int uvSize = ((width + 1) / 2) * ((height + 1) / 2);

                    const uint8_t* srcY = data;
                    const uint8_t* srcU = data + ySize;
                    const uint8_t* srcV = data + ySize + uvSize;

                    if (dstStrideY == width && dstStrideU == (width + 1) / 2 && dstStrideV == (width + 1) / 2) {
                        fastCopy(dstY, srcY, ySize);
                        fastCopy(dstU, srcU, uvSize);
                        fastCopy(dstV, srcV, uvSize);
                    }
                    else {
         
                        const int halfWidth = (width + 1) / 2;
                        const int halfHeight = (height + 1) / 2;

                        libyuv::CopyPlane(srcY, width, dstY, dstStrideY, width, height);
                        libyuv::CopyPlane(srcU, halfWidth, dstU, dstStrideU, halfWidth, halfHeight);
                        libyuv::CopyPlane(srcV, halfWidth, dstV, dstStrideV, halfWidth, halfHeight);
                    }
                }
                else {
                    // CPU 路径：需要颜色空间转换 (BGRA -> I420)
                    libyuv::ARGBToI420(
                        data, stride,       // 源数据和源 Stride (RowPitch)
                        dstY, dstStrideY,   // 目标 Y
                        dstU, dstStrideU,   // 目标 U
                        dstV, dstStrideV,   // 目标 V
                        width, height
                    );
                }

                webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(i420Buffer)
                    .set_rotation(webrtc::kVideoRotation_0)
                    .set_timestamp_us(webrtc::TimeMicros())
                    .build();

                videoTrackSourceImpl->PushFrame(frame);

                });

            if (!screenCapture->initialize()) {
                LOG_ERROR("Failed to initialize screen capture");
                return false;
            }

            if (!screenCapture->startCapture()) {
                LOG_ERROR("Failed to start screen capture");
                return false;
            }

            cursorHooks = std::make_unique<CursorHooks>();

            cursorHooks->setCursorHandler([this](unsigned char* data, size_t size) {

                if (!dataChannel) {

                    delete[] data;

                    return;

                }

                webrtc::CopyOnWriteBuffer buffer(data, size);

                webrtc::DataBuffer dataBuffer(buffer, true); // true 表示二进制数据

                dataChannel->SendAsync(dataBuffer, [this, data](webrtc::RTCError) {

                    delete[] data;

                    });

                });

            cursorHooks->startHooks();

            return true;
        }

        bool WebRTCManager::initializeHAudioCatch()
        {

            hAudioCatch = std::make_shared<HAudioCatch>();

            hAudioCatch->setDataHandle([this](unsigned char* data, size_t size) {

                if (!data) return;

                if (!audioDeviceModuleImpl) return;

                audioDeviceModuleImpl->PushAudioData(data, size);

                });

            if (!hAudioCatch->initlize()) {

                LOG_ERROR("HAudioCatch Initlize Failed!");

            }

            if (!hAudioCatch->runEventLoop()) {

                LOG_ERROR("HAudioCatch Run Event Loop Failed!");

                return false;

            }

            return true;
        }

        void WebRTCManager::handleDataChannelData(const unsigned char* data, size_t size)
        {
            if (size < sizeof(short)) {
                return;
            }

            const short eventType = *reinterpret_cast<const short*>(data);

            // 缓存屏幕分辨率
            thread_local static const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            thread_local static const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            switch (eventType) {
            case 0: { // Mouse move
                if (size < sizeof(short) + 2 * sizeof(uint16_t)) return;

#pragma pack(push,1)
                struct MouseMove              // 6 字节
                {
                    short  type;              // 0
                    uint16_t x;               // 屏幕绝对像素
                    uint16_t y;
                    uint32_t sequence;
                };
#pragma pack(pop)

                const MouseMove* mouseMove = reinterpret_cast<const MouseMove*>(data);

                if (mouseMove->sequence <= lastMouseSequence) {
                    // 可选：如果是刚启动第一次收到，可能需要特殊处理，
                    // 但通常 0 会被第一帧覆盖，所以直接 return 即可
                    return;
                }

                lastMouseSequence = mouseMove->sequence;

                keyMouseSim->MouseMove(mouseMove->x, mouseMove->y, true);

                break;
            }

            case 1:  // Mouse button down
            case 2: { // Mouse button up
                constexpr std::size_t kMinSize = sizeof(short) * 2 + 2 * sizeof(int);
                if (size < kMinSize) return;

                // 一次取完所有数据
                const auto* p = reinterpret_cast<const int16_t*>(data);
                const int16_t  mouseType = p[1];               // 2 字节
                const int32_t* coordPtr = reinterpret_cast<const int32_t*>(p + 2); // 8 字节

                // 0-65535 固定点 → 屏幕坐标，一次 64-bit 乘
                const uint32_t scaleX = (static_cast<uint64_t>(coordPtr[0]) * screenWidth) >> 16;
                const uint32_t scaleY = (static_cast<uint64_t>(coordPtr[1]) * screenHeight) >> 16;

                if (eventType == 1)
                    keyMouseSim->MouseButtonDown(mouseType, scaleX, scaleY);
                else
                    keyMouseSim->MouseButtonUp(mouseType);
                break;
            }

            case 3: // Key down
            case 4: { // Key up
                if (size < sizeof(short) + 2) {
                    return;
                }

                const unsigned char* keyData = data + sizeof(short);

                if (eventType == 3) {
                    keyMouseSim->KeyDown(keyData[0], keyData[1]);
                }
                else {
                    keyMouseSim->KeyUp(keyData[0], keyData[1]);
                }
                break;
            }

            case 5: { // Mouse wheel
                if (size < sizeof(short) + sizeof(int)) {
                    return;
                }

                keyMouseSim->MouseWheel(*reinterpret_cast<const int*>(data + sizeof(short)));
                break;
            }

            default:
                break;
            }
        }

        WebRTCManager::~WebRTCManager() {
            Cleanup();
        }

        // Add releaseSource implementation
        void WebRTCManager::releaseSource() {
            // Stop screen capture first
            if (screenCapture) {
                screenCapture.reset();
            }

            // Close peer connection
            if (peerConnection) {
                peerConnection->Close();
                peerConnection = nullptr;
            }

            // Reset observers
            if (peerConnectionObserver) {
                peerConnectionObserver.reset();
            }

            if (dataChannelObserver) {
                dataChannelObserver.reset();
            }

            if (createOfferObserver) {
                createOfferObserver = nullptr;
            }

            if (createAnswerObserver) {
                createAnswerObserver = nullptr;
            }

            // Reset tracks
            if (videoTrack) {
                videoTrack = nullptr;
            }

            if (videoSender) {
                videoSender = nullptr;
            }

            if (audioTrack) {
                audioTrack = nullptr;
            }

            if (audioSender) {
                audioSender = nullptr;
            }

            if (dataChannel) {
                dataChannel = nullptr;
            }

            if (videoTrackSourceImpl) {
                videoTrackSourceImpl = nullptr;
            }

            if (audioDeviceModuleImpl) {
                audioDeviceModuleImpl = nullptr;
            }

            // Reset factory
            if (peerConnectionFactory) {
                peerConnectionFactory = nullptr;
            }

            // Reset state flags
            isInit = false;

            isProcessingOffer = false;
        }

        void WebRTCManager::Cleanup() {
            socketRuns = false;

            releaseSource();

            if (writerChannel.is_open()) {
                writerChannel.close();
            }

            if (tcpSocket && tcpSocket->is_open()) {
                boost::system::error_code ec;
                tcpSocket->close(ec);
            }

            if (ioContextWorkPtr) {

                ioContextWorkPtr.reset();

            }

            ioContext.stop();

            if (ioContextThread.joinable()) {

                ioContextThread.join();

            }

            if (networkThread) {

                networkThread->Quit();

                networkThread.reset();

            }

            if (workerThread) {

                workerThread->Quit();

                workerThread.reset();

            }

            if (signalingThread) {

                signalingThread->Quit();

                signalingThread.reset();

            }

            if (cursorHooks) {

                cursorHooks->stopHooks();

                cursorHooks.reset();

            }

            if (hAudioCatch) {

                hAudioCatch->stopEventLoop();

                hAudioCatch.reset();

            }

            webrtc::CleanupSSL();
        }
    }

}