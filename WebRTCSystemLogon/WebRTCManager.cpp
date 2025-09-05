#include "WebRTCManager.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/video/i420_buffer.h>

// Observer实现
void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
    switch (new_state) {
    case webrtc::PeerConnectionInterface::kClosed:
        Logger::getInstance()->info("Signaling state: kClosed");
        break;
    default:
        break;
    }
}

void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
}

void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    if (new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
        Logger::getInstance()->info("ICE gathering complete");
    }
}

void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    if (!candidate) {
        Logger::getInstance()->error("OnIceCandidate called with null candidate");
        return;
    }

    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert ICE candidate to string");
        return;
    }

    boost::json::object msg;
    msg["type"] = "candidate";
    msg["candidate"] = sdp;
    msg["mid"] = candidate->sdp_mid();
    msg["mlineIndex"] = candidate->sdp_mline_index();

    manager_->sendSignalingMessage(msg);
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
        Logger::getInstance()->info("ICE connection established");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
        Logger::getInstance()->error("ICE connection failed");
        break;
    default:
        break;
    }
}

void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    switch (new_state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
        Logger::getInstance()->info("Peer connection established");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
        Logger::getInstance()->error("Peer connection failed");
        break;
    default:
        break;
    }
}

void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer)
{
    if (buffer.size() == 0) {
        return;
    }

    if (buffer.size() > 1024 * 1024) { // 1MB limit
        Logger::getInstance()->error("Data channel message too large: " + std::to_string(buffer.size()));
        return;
    }

    const std::byte* data = reinterpret_cast<const std::byte*>(buffer.data.data());
    size_t size = buffer.size();

    manager->processDataChannelMessage(std::vector<std::byte>(data, data + size));
}

// SetLocalDescriptionObserver实现
void SetLocalDescriptionObserver::OnSuccess() {
}

void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetLocalDescription failed: " + std::string(error.message()));
}

// SetRemoteDescriptionObserver实现
void SetRemoteDescriptionObserver::OnSuccess() {
}

void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetRemoteDescription failed: " + std::string(error.message()));
}

// CreateOfferObserverImpl实现
void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc) {
        Logger::getInstance()->error("CreateOffer success callback received null description");
        manager_->isProcessingOffer = false;
        return;
    }

    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert offer to string");
        manager_->isProcessingOffer = false;
        return;
    }

    boost::json::object msg;
    msg["type"] = "offer";
    msg["sdp"] = sdp;

    manager_->sendSignalingMessage(msg);
}

void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateOffer failed: " + std::string(error.message()));
    manager_->isProcessingOffer = false;
}

// CreateAnswerObserverImpl实现
void CreateAnswerObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc) {
        Logger::getInstance()->error("CreateAnswer success callback received null description");
        return;
    }

    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert answer to string");
        return;
    }

    boost::json::object msg;
    msg["type"] = "answer";
    msg["sdp"] = sdp;

    manager_->sendSignalingMessage(msg);
}

void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateAnswer failed: " + std::string(error.message()));
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

    if (dataChannel) {
        dataChannel = nullptr;
    }

    if (videoTrackSourceImpl) {
        videoTrackSourceImpl = nullptr;
    }

    // Reset factory
    if (peerConnectionFactory) {
        peerConnectionFactory = nullptr;
    }

    // Reset state flags
    isInit = false;
    isProcessingOffer = false;
}

WebRTCManager::WebRTCManager(WebRTCVideoCodec codec)
    : tcpSocket(std::make_unique<boost::asio::ip::tcp::socket>(ioContext)),
    accept(socketIoContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), 19998)),
    state(WebRTCRemoteState::nullRemote),
    connetState(WebRTCConnetState::none),
    peerConnection(nullptr),
    writerChannel(socketIoContext),
    winLogon(nullptr),
    keyMouseSim(nullptr),
    codec(codec),
    inputInjector(nullptr) {

    Logger::getInstance()->info("WebRTCManager starting on port 19998");

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this]() {
        this->ioContext.run();
        }));

    socketIoContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(socketIoContext));

    socketIoContextThread = std::move(std::thread([this]() {
        this->socketIoContext.run();
        }));

    boost::asio::co_spawn(socketIoContext, [this]()-> boost::asio::awaitable<void> {
        tcpSocket = std::make_unique<boost::asio::ip::tcp::socket>(socketIoContext);
        co_await accept.async_accept(*tcpSocket, boost::asio::use_awaitable);
        Logger::getInstance()->info("TCP connection accepted");
        socketEventLoop();
        }, [this](std::exception_ptr p) {
            try {
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("TCP acceptor error: " + std::string(e.what()));
            }
            });

        keyMouseSim = std::make_unique<KeyMouseSimulator>();

        if (!keyMouseSim->Initialize()) {
            Logger::getInstance()->error("KeyMouseSimulator initialization failed");
        }

        inputInjector = winrt::Windows::UI::Input::Preview::Injection::InputInjector::TryCreate();

        if (!inputInjector) {
            Logger::getInstance()->warning("InputInjector creation failed, using KeyMouseSimulator");
        }

}

void WebRTCManager::sendSignalingMessage(const boost::json::object& message) {
    boost::json::object fullMsg;
    fullMsg["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
    fullMsg["accountID"] = accountID;
    fullMsg["targetID"] = targetID;
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
        Logger::getInstance()->error("Received empty SDP offer");
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
        Logger::getInstance()->error("Failed to parse offer: " + error.description);
        isInit = false;
    }
}

void WebRTCManager::processAnswer(const std::string& sdp) {
    if (sdp.empty()) {
        Logger::getInstance()->error("Received empty SDP answer");
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
        Logger::getInstance()->error("Failed to parse answer: " + error.description);
        isInit = false;
    }
}

void WebRTCManager::processIceCandidate(const std::string& candidate,
    const std::string& mid, int lineIndex) {
    if (candidate.empty()) {
        return;
    }

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
        webrtc::CreateIceCandidate(mid, lineIndex, candidate, &error));

    if (ice_candidate) {
        peerConnection->AddIceCandidate(ice_candidate.release());
    }
    else {
        Logger::getInstance()->error("Failed to parse ICE candidate: " + error.description);
    }
}

void WebRTCManager::socketEventLoop() {
    socketRuns = true;

    // 读取协程
    boost::asio::co_spawn(socketIoContext, [this]() -> boost::asio::awaitable<void> {
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
                        Logger::getInstance()->error("Socket read error: " + std::string(e.what()));
                        co_return;
                    }
                }

                int64_t rawBodyLength = 0;
                std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
                int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

                if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) {
                    Logger::getInstance()->error("Invalid body length: " + std::to_string(bodyLength));
                    co_return;
                }

                size_t bodySize = static_cast<size_t>(bodyLength);

                std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]);
                if (!bodyBuffer) {
                    Logger::getInstance()->error("Failed to allocate buffer");
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
                        Logger::getInstance()->error("Socket read error: " + std::string(e.what()));
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

                        if (WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER) {
                            if (responseState == 200) {
                                Logger::getInstance()->info("Registration successful");
                                connetState = WebRTCConnetState::connect;

                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to initialize peer connection");
                                }
                            }
                            else {
                                Logger::getInstance()->error("Registration failed");
                            }
                        }
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST) {
                            if (responseState == 200) {
                                if (json.contains("webRTCRemoteState")) {
                                    WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());

                                    if (state.load() == WebRTCRemoteState::nullRemote) {
                                        if (remoteState == WebRTCRemoteState::masterRemote) {
                                            state = WebRTCRemoteState::followerRemote;

                                            if (!initializePeerConnection()) {
                                                Logger::getInstance()->error("Failed to initialize peer connection");
                                                continue;
                                            }

                                            if (!initializeScreenCapture()) {
                                                Logger::getInstance()->error("Failed to initialize screen capture");
                                                continue;
                                            }

                                            if (json.contains("accountID")) {
                                                targetID = std::string(json["accountID"].as_string().c_str());
                                            }

                                            if (json.contains("targetID")) {
                                                accountID = std::string(json["targetID"].as_string().c_str());
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
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::RESTART) {
                            if (responseState == 200) {
                                releaseSource();
                                state = WebRTCRemoteState::nullRemote;

                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to reinitialize after restart");
                                }
                            }
                        }
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE) {
                            if (responseState == 200) {
                                releaseSource();
                                state = WebRTCRemoteState::nullRemote;

                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to reinitialize after stop");
                                }
                            }
                        }
                    }
                }
                catch (const std::exception& e) {
                    Logger::getInstance()->error("JSON parse error: " + std::string(e.what()));
                }
            }
        }
        catch (const std::exception& e) {
            Logger::getInstance()->error("Reader coroutine error: " + std::string(e.what()));
        }

        co_return;

        }, [this](std::exception_ptr p) {
            try {
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("Reader coroutine exception: " + std::string(e.what()));
            }
            });

        // 写入协程
        boost::asio::co_spawn(socketIoContext, [this]() -> boost::asio::awaitable<void> {
            try {
                for (;;) {
                    while (this->writerDataQueues.size_approx() == 0 && this->socketRuns.load()) {
                        co_await this->writerChannel.async_receive(boost::asio::use_awaitable);
                    }

                    if (!socketRuns) {
                        while (this->writerDataQueues.size_approx() > 0) {
                            std::shared_ptr<WriterData> nowNode = nullptr;
                            if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                                try {
                                    co_await boost::asio::async_write(*this->tcpSocket,
                                        boost::asio::buffer(nowNode->data, nowNode->size),
                                        boost::asio::use_awaitable);
                                }
                                catch (const std::exception& e) {
                                    break;
                                }
                            }
                        }
                        co_return;
                    }

                    std::shared_ptr<WriterData> nowNode = nullptr;
                    if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                        try {
                            co_await boost::asio::async_write(*this->tcpSocket,
                                boost::asio::buffer(nowNode->data, nowNode->size),
                                boost::asio::use_awaitable);
                        }
                        catch (const boost::system::system_error& e) {
                            Logger::getInstance()->error("Socket write error: " + std::string(e.what()));
                            break;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("Writer coroutine error: " + std::string(e.what()));
            }

            co_return;

            }, [this](std::exception_ptr p) {
                try {
                    if (p) {
                        std::rethrow_exception(p);
                    }
                }
                catch (const std::exception& e) {
                    Logger::getInstance()->error("Writer coroutine exception: " + std::string(e.what()));
                }
                });
}

void WebRTCManager::writerAsync(std::shared_ptr<WriterData> data) {
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

    networkThread = webrtc::Thread::CreateWithSocketServer();
    if (!networkThread) {
        Logger::getInstance()->error("Failed to create network thread");
        return false;
    }
    networkThread->SetName("network_thread", nullptr);
    if (!networkThread->Start()) {
        Logger::getInstance()->error("Failed to start network thread");
        return false;
    }

    workerThread = webrtc::Thread::Create();
    if (!workerThread) {
        Logger::getInstance()->error("Failed to create worker thread");
        return false;
    }
    workerThread->SetName("worker_thread", nullptr);
    if (!workerThread->Start()) {
        Logger::getInstance()->error("Failed to start worker thread");
        return false;
    }

    signalingThread = webrtc::Thread::Create();
    if (!signalingThread) {
        Logger::getInstance()->error("Failed to create signaling thread");
        return false;
    }
    signalingThread->SetName("signaling_thread", nullptr);
    if (!signalingThread->Start()) {
        Logger::getInstance()->error("Failed to start signaling thread");
        return false;
    }

    peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
        networkThread.get(),
        workerThread.get(),
        signalingThread.get(),
        nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (!peerConnectionFactory) {
        Logger::getInstance()->error("Failed to create PeerConnectionFactory");
        return false;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

    webrtc::PeerConnectionInterface::IceServer stun_server;
    stun_server.uri = "stun:14.103.170.36:3478";
    config.servers.push_back(stun_server);

    webrtc::PeerConnectionInterface::IceServer turn_server;
    turn_server.uri = "turn:14.103.170.36:3478";
    turn_server.username = "HopeTiga";
    turn_server.password = "dy913140924";
    config.servers.emplace_back(turn_server);

    peerConnectionObserver = std::make_unique<PeerConnectionObserverImpl>(this);

    webrtc::PeerConnectionDependencies pc_dependencies(peerConnectionObserver.get());

    auto pcResult = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pc_dependencies));
    if (!pcResult.ok()) {
        Logger::getInstance()->error("Failed to create PeerConnection: " + std::string(pcResult.error().message()));
        return false;
    }

    peerConnection = pcResult.MoveValue();

    // 如果是发送端，创建视频源和轨道
    if (state == WebRTCRemoteState::followerRemote) {
        videoTrackSourceImpl = webrtc::make_ref_counted<VideoTrackSourceImpl>();

        videoTrack = peerConnectionFactory->CreateVideoTrack(videoTrackSourceImpl, "videoTrack");

        if (!videoTrack) {
            Logger::getInstance()->error("Failed to create video track");
            return false;
        }

        std::vector<webrtc::RtpEncodingParameters> encodings;
        webrtc::RtpEncodingParameters encoding;

        encoding.active = true;
        encoding.max_bitrate_bps = 4000000;  // 4 Mbps
        encoding.min_bitrate_bps = 750000;   // 500 kbps
        encoding.max_framerate = 75;
        encoding.scale_resolution_down_by = 1.0;
        encoding.scalability_mode = "L1T1";

        encodings.push_back(encoding);

        std::vector<std::string> stream_ids = { "mediaStream" };
        auto addTrackResult = peerConnection->AddTrack(videoTrack, stream_ids, encodings);

        if (!addTrackResult.ok()) {
            Logger::getInstance()->error("Failed to add video track: " + std::string(addTrackResult.error().message()));
            return false;
        }

        videoSender = addTrackResult.MoveValue();

        auto transceivers = peerConnection->GetTransceivers();

        for (auto& transceiver : transceivers) {
            if (transceiver->media_type() == webrtc::MediaType::MEDIA_TYPE_VIDEO) {
                auto codecs = transceiver->codec_preferences();
                std::vector<webrtc::RtpCodecCapability> preferred_codecs;

                // 根据枚举选择优先编解码器
                std::string priority_codec;
                switch (this->codec) {
                case WebRTCVideoCodec::VP9: priority_codec = "VP9"; break;
                case WebRTCVideoCodec::H264: priority_codec = "H264"; break;
                case WebRTCVideoCodec::VP8: priority_codec = "VP8"; break;
                }

                // 找到优先编解码器并移到最前
                for (auto it = codecs.begin(); it != codecs.end(); ++it) {
                    if (it->name == priority_codec) {
                        preferred_codecs.push_back(*it);
                        codecs.erase(it);
                        break;
                    }
                }

                // 添加剩余编解码器
                for (const auto& codec : codecs) {
                    if (codec.name != "red" && codec.name != "ulpfec") {
                        preferred_codecs.push_back(codec);
                    }
                }

                // 设置新偏好
                transceiver->SetCodecPreferences(preferred_codecs);
            }
        }

        webrtc::RtpParameters parameters = videoSender->GetParameters();

        if (!parameters.encodings.empty()) {
            parameters.encodings[0] = encoding;
        }
        else {
            parameters.encodings = encodings;
        }

        parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;

        auto setParamsResult = videoSender->SetParameters(parameters);

        if (!setParamsResult.ok()) {
            Logger::getInstance()->error("Failed to set RTP parameters: " + std::string(setParamsResult.message()));
            return false;
        }
    }

    std::unique_ptr<webrtc::DataChannelInit> dataChannelConfig = std::make_unique<webrtc::DataChannelInit>();

    dataChannel = peerConnection->CreateDataChannel("dataChannel", dataChannelConfig.get());
    if (!dataChannel) {
        Logger::getInstance()->error("Failed to create data channel");
        return false;
    }

    dataChannelObserver = std::make_unique<DataChannelObserverImpl>(this);
    dataChannel->RegisterObserver(dataChannelObserver.get());

    Logger::getInstance()->info("PeerConnection initialized");
    return true;
}

bool WebRTCManager::initializeScreenCapture() {
    if (screenCapture) {
        return false;
    }

    screenCapture = std::make_shared<ScreenCapture>();

    screenCapture->setFrameCallback([this](const uint8_t* data, size_t size, int width, int height) {

        if (!videoTrackSourceImpl || !data || size == 0) {
            return;
        }

        size_t expectedSize = width * height * 3 / 2;
        if (size != expectedSize) {
            return;
        }

        webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer =
            webrtc::I420Buffer::Create(width, height);

        if (!i420Buffer) {
            return;
        }

        const int ySize = width * height;
        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;
        const int uvSize = uvWidth * uvHeight;

        memcpy(i420Buffer->MutableDataY(), data, ySize);
        memcpy(i420Buffer->MutableDataU(), data + ySize, uvSize);
        memcpy(i420Buffer->MutableDataV(), data + ySize + uvSize, uvSize);

        int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420Buffer)
            .set_timestamp_us(timestamp_us)
            .build();

        videoTrackSourceImpl->PushFrame(frame);
        });

    if (!screenCapture->initialize()) {
        Logger::getInstance()->error("Failed to initialize screen capture");
        return false;
    }

    if (!screenCapture->startCapture()) {
        Logger::getInstance()->error("Failed to start screen capture");
        return false;
    }

    Logger::getInstance()->info("Screen capture started");
    return true;
}

void WebRTCManager::processDataChannelMessage(const std::vector<std::byte>& bytes)
{
    if (bytes.empty()) {
        return;
    }

    if (bytes.size() < sizeof(short)) {
        return;
    }

    const unsigned char* buffer = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t size = bytes.size();

    short eventType = 0;
    std::memcpy(&eventType, buffer, sizeof(short));

    // 获取当前屏幕分辨率
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    switch (eventType) {
    case 0: { // Mouse move
        if (size < sizeof(short) + 2 * sizeof(int)) {
            return;
        }

        int normalizedX = 0;
        int normalizedY = 0;

        std::memcpy(&normalizedX, buffer + sizeof(short), sizeof(int));
        std::memcpy(&normalizedY, buffer + sizeof(short) + sizeof(int), sizeof(int));

        // 将归一化坐标转换为本地屏幕坐标
        int posX = (normalizedX * screenWidth) / 65535;
        int posY = (normalizedY * screenHeight) / 65535;

        // 确保坐标在有效范围内
        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        if (!keyMouseSim->MouseMove(posX, posY, true)) {
            Logger::getInstance()->error("Failed to move mouse");
        }
        break;
    }

    case 1: { // Mouse button down
        if (size < sizeof(short) * 2 + 2 * sizeof(int)) {
            return;
        }

        short mouseType = 0;
        int normalizedX = 0;
        int normalizedY = 0;

        std::memcpy(&mouseType, buffer + sizeof(short), sizeof(short));
        std::memcpy(&normalizedX, buffer + sizeof(short) * 2, sizeof(int));
        std::memcpy(&normalizedY, buffer + sizeof(short) * 2 + sizeof(int), sizeof(int));

        // 将归一化坐标转换为本地屏幕坐标
        int posX = (normalizedX * screenWidth) / 65535;
        int posY = (normalizedY * screenHeight) / 65535;

        // 确保坐标在有效范围内
        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        if (!keyMouseSim->MouseButtonDown(mouseType, posX, posY)) {
            Logger::getInstance()->error("Failed to send mouse button down");
        }
        break;
    }

    case 2: { // Mouse button up
        if (size < sizeof(short) * 2 + 2 * sizeof(int)) {
            return;
        }

        short mouseType = 0;
        int normalizedX = 0;
        int normalizedY = 0;

        std::memcpy(&mouseType, buffer + sizeof(short), sizeof(short));
        std::memcpy(&normalizedX, buffer + sizeof(short) * 2, sizeof(int));
        std::memcpy(&normalizedY, buffer + sizeof(short) * 2 + sizeof(int), sizeof(int));

        // 将归一化坐标转换为本地屏幕坐标
        int posX = (normalizedX * screenWidth) / 65535;
        int posY = (normalizedY * screenHeight) / 65535;

        // 确保坐标在有效范围内
        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        if (!keyMouseSim->MouseButtonUp(mouseType)) {
            Logger::getInstance()->error("Failed to send mouse button up");
        }
        break;
    }

    case 3: { // Key down
        if (size < sizeof(short) + 2 * sizeof(char)) {
            return;
        }

        unsigned char windowsKey = 0;
        char modifiers = 0;

        std::memcpy(&windowsKey, buffer + sizeof(short), sizeof(char));
        std::memcpy(&modifiers, buffer + sizeof(short) + sizeof(char), sizeof(char));

        bool needsShift = false;
        unsigned char actualKey = windowsKey;

        if (needsShift) {
            modifiers |= 0x01;
        }

        if (!keyMouseSim->KeyDown(windowsKey, modifiers)) {
            Logger::getInstance()->error("Failed to send key down");
        }

        return;

        break;
    }

    case 4: { // Key up
        if (size < sizeof(short) + 2 * sizeof(char)) {
            return;
        }

        unsigned char windowsKey = 0;
        char modifiers = 0;

        std::memcpy(&windowsKey, buffer + sizeof(short), sizeof(char));
        std::memcpy(&modifiers, buffer + sizeof(short) + sizeof(char), sizeof(char));

        bool needsShift = false;
        unsigned char actualKey = windowsKey;

        if (needsShift) {
            modifiers |= 0x01;
        }

        if (!keyMouseSim->KeyUp(windowsKey, modifiers)) {
            Logger::getInstance()->error("Failed to send key up");
        }
        return;

        break;
    }

    case 5: { // Mouse wheel
        if (size < sizeof(short) + sizeof(int)) {
            return;
        }

        int wheelValue = 0;
        std::memcpy(&wheelValue, buffer + sizeof(short), sizeof(int));

        if (!keyMouseSim->MouseWheel(wheelValue)) {
            Logger::getInstance()->error("Failed to send mouse wheel");
        }

        break;
    }

    default:
        break;
    }

}



// processDataChannelMessage中的case 3和case 4保持不变，但确保日志正确
WebRTCManager::~WebRTCManager() {
    Cleanup();
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

    if (socketIoContextWorkPtr) {
        socketIoContextWorkPtr.reset();
    }

    if (ioContextThread.joinable()) {
        ioContextThread.join();
    }

    if (socketIoContextThread.joinable()) {
        socketIoContextThread.join();
    }

    if (networkThread) {
        networkThread->Stop();
    }

    if (workerThread) {
        workerThread->Stop();
    }

    if (signalingThread) {
        signalingThread->Stop();
    }

    webrtc::CleanupSSL();
}