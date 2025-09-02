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

    manager->ioContext.post([=]() mutable {
        manager->processDataChannelMessage(std::vector<std::byte>(data, data + size));
        });
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

void WebRTCManager::disConnect()
{
    webSocketRuns = false;

    if (webSocket && webSocket->is_open()) {

        try {

            webSocket->next_layer().cancel();

            webSocket->close(boost::beast::websocket::close_code::normal);

        }
        catch (std::exception& e) {

            Logger::getInstance()->info(e.what());

        }
    }


    releaseSource();

    connetState = WebRTCConnetState::none;

}

void WebRTCManager::disConnectRemote()
{
    releaseSource();

    initializePeerConnection();

    this->state = WebRTCRemoteState::nullRemote;

    if (webSocket && webSocket->is_open()) {
        boost::json::object message;
        message["accountID"] = this->accountID;
        message["targetID"] = this->targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);
        std::string messageStr = boost::json::serialize(message);
        webSocket->write(boost::asio::buffer(messageStr));
    }
}

void WebRTCManager::writerVideoFrame(std::byte* data, size_t size, int width, int height)
{
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
}

void WebRTCManager::writerAudioFrame(std::byte* data, size_t size)
{
    if (audioDeviceModuleImpl) {
        audioDeviceModuleImpl->PushAudioData(data, size);
    }
}



// Add releaseSource implementation
void WebRTCManager::releaseSource() {
    // Stop screen capture first


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

WebRTCManager::WebRTCManager(HWND windowHandle)
	: webSocket(nullptr), resolver(nullptr),
    state(WebRTCRemoteState::nullRemote),
    connetState(WebRTCConnetState::none),
    peerConnection(nullptr),
	windowHandle(windowHandle)
 {

    Logger::getInstance()->info("WebRTCManager start");

    webSocket = std::make_unique<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>>(ioContext);

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this]() {
        this->ioContext.run();
        }));

    
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
    
    webSocket->write(boost::asio::buffer(msgStr));
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


void WebRTCManager::setAccountID(std::string accountID)
{
    this->accountID = accountID;
}

std::string WebRTCManager::getAccountID()
{
    return this->accountID ;
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

    audioDeviceModuleImpl = AudioDeviceModuleImpl::Create();

    peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
        networkThread.get(),
        workerThread.get(),
        signalingThread.get(),
        audioDeviceModuleImpl,
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
    encoding.min_bitrate_bps = 500000;   // 500 kbps
    encoding.max_framerate = 60;
    encoding.scale_resolution_down_by = 1.0;
    encoding.scalability_mode = "L1T1";

    encodings.push_back(encoding);


    std::vector<std::string> videoStreamIds = { "videoStream" };

    auto addTrackResult = peerConnection->AddTrack(videoTrack, videoStreamIds, encodings);

    if (!addTrackResult.ok()) {
        Logger::getInstance()->error("Failed to add video track: " + std::string(addTrackResult.error().message()));
        return false;
    }

    videoSender = addTrackResult.MoveValue();

    // Configure H264 codec
    auto transceivers = peerConnection->GetTransceivers();
    for (auto& transceiver : transceivers) {
        if (transceiver->media_type() == webrtc::MediaType::MEDIA_TYPE_VIDEO) {
            auto codecs = transceiver->codec_preferences();

            std::vector<webrtc::RtpCodecCapability> preferred_codecs;
            webrtc::RtpCodecCapability h264_codec;
            bool h264_found = false;

            for (const auto& codec : codecs) {
                if (codec.name == "H264") {
                    h264_codec = codec;
                    h264_codec.parameters["profile-level-id"] = "42e01f";
                    h264_codec.parameters["level-asymmetry-allowed"] = "1";
                    h264_codec.parameters["packetization-mode"] = "1";
                    preferred_codecs.clear();
                    preferred_codecs.insert(preferred_codecs.begin(), h264_codec);
                    h264_found = true;
                }
                else if (codec.name != "red" && codec.name != "ulpfec") {
                    preferred_codecs.push_back(codec);
                }
            }

            if (h264_found) {
                auto result = transceiver->SetCodecPreferences(preferred_codecs);
                if (!result.ok()) {
                    Logger::getInstance()->error("Failed to set codec preferences: " + std::string(result.message()));
                    return false;
                }
            }
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

    audioTrack = peerConnectionFactory->CreateAudioTrack("audioTrack", nullptr);

    std::vector<std::string> audioStreamIds = { "audioStream" };

    auto addAudioTrackResult = peerConnection->AddTrack(audioTrack, audioStreamIds, encodings);

    if (!addAudioTrackResult.ok()) {
        Logger::getInstance()->error("Failed to add video track: " + std::string(addTrackResult.error().message()));
        return false;
    }

    audioSender = addAudioTrackResult.MoveValue();

    // 配置音频Opus编码参数
    transceivers = peerConnection->GetTransceivers(); // 重新获取，因为添加了新轨道
    for (auto& transceiver : transceivers) {
        if (transceiver->media_type() == webrtc::MediaType::MEDIA_TYPE_AUDIO) {
            // 设置Opus为首选编码器
            auto audioCodecs = transceiver->codec_preferences();
            std::vector<webrtc::RtpCodecCapability> preferredAudioCodecs;

            for (const auto& codec : audioCodecs) {
                if (codec.name == "opus") {
                    webrtc::RtpCodecCapability opusCodec = codec;
                    // 设置Opus参数
                    opusCodec.parameters["stereo"] = "1";           // 启用立体声
                    opusCodec.parameters["sprop-stereo"] = "1";     // 声明立体声支持
                    opusCodec.parameters["maxplaybackrate"] = "48000"; // 最大播放采样率
                    opusCodec.parameters["maxaveragebitrate"] = "128000"; // 平均比特率
                    opusCodec.parameters["cbr"] = "0";              // 使用VBR（可变比特率）
                    opusCodec.parameters["useinbandfec"] = "1";     // 启用前向纠错
                    opusCodec.parameters["usedtx"] = "0";           // 禁用不连续传输
                    opusCodec.parameters["ptime"] = "20";           // 20ms包时长

                    preferredAudioCodecs.insert(preferredAudioCodecs.begin(), opusCodec);
                    break;
                }
            }

            // 添加其他编码器作为备选
            for (const auto& codec : audioCodecs) {
                if (codec.name != "opus" && codec.name != "telephone-event") {
                    preferredAudioCodecs.push_back(codec);
                }
            }

            auto result = transceiver->SetCodecPreferences(preferredAudioCodecs);
            if (!result.ok()) {
                Logger::getInstance()->error("Failed to set audio codec preferences: " + std::string(result.message()));
            }

            // 设置音频发送参数
            webrtc::RtpParameters audioParams = audioSender->GetParameters();
            if (!audioParams.encodings.empty()) {
                audioParams.encodings[0].active = true;
                audioParams.encodings[0].max_bitrate_bps = 128000;
                audioParams.encodings[0].network_priority = webrtc::Priority::kHigh;
            }

            auto setAudioParamsResult = audioSender->SetParameters(audioParams);
            if (!setAudioParamsResult.ok()) {
                Logger::getInstance()->error("Failed to set audio RTP parameters: " +
                    std::string(setAudioParamsResult.message()));
            }
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


void WebRTCManager::connect(std::string ip)
{
    std::string host = ip;
    std::string port = "8080"; // 默认端口，你可以根据需要修改

    // 如果 IP 包含端口号（格式：ip:port）
    size_t colonPos = ip.find(':');
    if (colonPos != std::string::npos) {
        host = ip.substr(0, colonPos);
        port = ip.substr(colonPos + 1);
    }

    if (!resolver) {
        resolver = std::make_unique<boost::asio::ip::tcp::resolver>(ioContext);
    }

    // 解析主机和端口
    auto results = resolver->resolve(host, port);

    // 确保 webSocket 已初始化
    if (!webSocket) {
        Logger::getInstance()->info("WebRTCManager webSocket init faild ");
        return;
    }

    // 连接到服务器

    boost::system::error_code ec;

    boost::asio::connect(webSocket->next_layer(), results, ec);

    if (ec) {

        Logger::getInstance()->info("connect webSocketServer faild :" + ec.what());

        return;

    }

    // 执行 WebSocket 握手
    webSocket->handshake(host, "/", ec);

    if (ec) {

        Logger::getInstance()->info("handshake webSocketServer faild :" + ec.what());

        return;

    }


    webSocketRuns = true;

    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {

        boost::beast::flat_buffer buffer;

        try {
            while (webSocketRuns) {

                co_await  webSocket->async_read(buffer, boost::asio::use_awaitable);

                std::string dataStr = boost::beast::buffers_to_string(buffer.data());

                buffer.consume(buffer.size());

                //Logger::getInstance()->info(dataStr);

                boost::json::object json = boost::json::parse(dataStr).as_object();

                if (json.contains("requestType")) {
                    int64_t requestType = json["requestType"].as_int64();
                    int64_t responseState = json["state"].as_int64();

                    if (WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER) {
                        if (responseState == 200) {
                            connetState = WebRTCConnetState::connect;


                            if (!initializePeerConnection()) {
      
                            }
                        }
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST) {
                        
                        if (responseState == 200) {
                         
                            if (json.contains("webRTCRemoteState")) {
                                WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());
                             
                                // 只在状态未设置时进行角色分配，避免重复处理
                                if (state.load() == WebRTCRemoteState::nullRemote) {
                                  
                                    if (remoteState == WebRTCRemoteState::masterRemote) {
                                        state = WebRTCRemoteState::followerRemote;
                                      
                                        if (json.contains("accountID")) {
                                            targetID = std::string(json["accountID"].as_string().c_str());
                                        }

                                        if (json.contains("targetID")) {
                                            accountID = std::string(json["targetID"].as_string().c_str());
                                        }

                                        if (!isProcessingOffer.exchange(true)) {
                                            
                                            webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                                            options.offer_to_receive_video = true;
                                            options.offer_to_receive_audio = true;

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
                        // 在处理重启消息的地方添加日志
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::RESTART) {

                        if (responseState == 200) {

                            releaseSource();

                            initializePeerConnection();

                            this->state = WebRTCRemoteState::nullRemote;

                        }
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE) {

                        if (responseState == 200) {

                            releaseSource();

                            initializePeerConnection();

                            this->state = WebRTCRemoteState::nullRemote;

                        }
                    }
                }
            }

        }
        catch (const std::exception& e) {

            Logger::getInstance()->error("webSocket Read coroutine Error: " + std::string(e.what()));
        }

        }, [this](std::exception_ptr p) {
            try {
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("webSocket Read coroutine Error: " + std::string(e.what()));
            }
            });

        boost::json::object request;

        request["requestType"] = static_cast<int>(WebRTCRequestState::REGISTER);

        request["accountID"] = this->accountID;

        std::string requestStr = boost::json::serialize(request);

        webSocket->write(boost::asio::buffer(requestStr));

        Logger::getInstance()->info("WebRTCManager websocket connet successful!");
}


void WebRTCManager::processDataChannelMessage(const std::vector<std::byte>& bytes)
{
    Logger::getInstance()->debug("Processing DataChannel message, size: " + std::to_string(bytes.size()));

    if (bytes.empty()) {
        Logger::getInstance()->warning("Received empty DataChannel message");
        return;
    }

    if (!windowHandle || !IsWindow(windowHandle)) {
        Logger::getInstance()->warning("Invalid window handle");
        return;
    }

    const unsigned char* buffer = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t size = bytes.size();

    // 基本大小检查
    if (size < sizeof(short)) {
        Logger::getInstance()->warning("DataChannel message too small");
        return;
    }

    // 分配并填充RemoteInputData结构
    RemoteInputData* inputData = new RemoteInputData();
    std::memcpy(&inputData->eventType, buffer, sizeof(short));

    Logger::getInstance()->debug("Event type: " + std::to_string(inputData->eventType));

    bool validData = true;

    switch (inputData->eventType) {
    case 0: { // Mouse move
        if (size >= sizeof(short) + 2 * sizeof(int)) {
            std::memcpy(&inputData->mouseMove.posX, buffer + sizeof(short), sizeof(int));
            std::memcpy(&inputData->mouseMove.posY, buffer + sizeof(short) + sizeof(int), sizeof(int));
        }
        else {
            validData = false;
        }
        break;
    }

    case 1: // Mouse button down
    case 2: { // Mouse button up
        if (size >= sizeof(short) * 2 + 2 * sizeof(int)) {
            std::memcpy(&inputData->mouseButton.mouseType, buffer + sizeof(short), sizeof(short));
            std::memcpy(&inputData->mouseButton.posX, buffer + sizeof(short) * 2, sizeof(int));
            std::memcpy(&inputData->mouseButton.posY, buffer + sizeof(short) * 2 + sizeof(int), sizeof(int));

        }
        else {
            validData = false;
        }
        break;
    }

    case 3: // Key down
    case 4: { // Key up
        if (size >= sizeof(short) + sizeof(char) * 2) {
            std::memcpy(&inputData->keyboard.windowsKey, buffer + sizeof(short), sizeof(char));
            std::memcpy(&inputData->keyboard.modifiers, buffer + sizeof(short) + sizeof(char), sizeof(char));
        }
        else {
            validData = false;
        }
        break;
    }

    case 5: { // Mouse wheel
        if (size >= sizeof(short) + sizeof(int)) {
            std::memcpy(&inputData->mouseWheel.wheelValue, buffer + sizeof(short), sizeof(int));
        }
        else {
            validData = false;
        }
        break;
    }

    default:
        Logger::getInstance()->warning("Unknown event type: " + std::to_string(inputData->eventType));
        validData = false;
        break;
    }

    if (validData) {
        // 发送到主线程处理
        if (!PostMessage(windowHandle, WM_REMOTE_INPUT, 0, reinterpret_cast<LPARAM>(inputData))) {
            Logger::getInstance()->error("Failed to post remote input message, error: " + std::to_string(GetLastError()));
            delete inputData; // 如果发送失败，清理内存
        }
        else {
            Logger::getInstance()->debug("Remote input message posted to main thread successfully");
        }
    }
    else {
        Logger::getInstance()->error("Invalid data structure for event type: " + std::to_string(inputData->eventType));
        delete inputData;
    }
}

void WebRTCManager::handleSymbolKeyMapping(unsigned char& windowsKey, bool& needsShift)
{
    needsShift = false;

    if (windowsKey >= VK_OEM_1 && windowsKey <= VK_OEM_8) {
        return;
    }

    if (windowsKey >= VK_LEFT && windowsKey <= VK_DOWN) {
        return;
    }

    switch (windowsKey) {
    case '!': windowsKey = 0x31; needsShift = true; break;
    case '@': windowsKey = 0x32; needsShift = true; break;
    case '#': windowsKey = 0x33; needsShift = true; break;
    case '$': windowsKey = 0x34; needsShift = true; break;
    case '%': windowsKey = 0x35; needsShift = true; break;
    case '^': windowsKey = 0x36; needsShift = true; break;
    case '&': windowsKey = 0x37; needsShift = true; break;
    case '*': windowsKey = 0x38; needsShift = true; break;
    case '(': windowsKey = 0x39; needsShift = true; break;
    case ')': windowsKey = 0x30; needsShift = true; break;
    case ':': windowsKey = VK_OEM_1; needsShift = true; break;
    case '+': windowsKey = VK_OEM_PLUS; needsShift = true; break;
    case '<': windowsKey = VK_OEM_COMMA; needsShift = true; break;
    case '_': windowsKey = VK_OEM_MINUS; needsShift = true; break;
    case '>': windowsKey = VK_OEM_PERIOD; needsShift = true; break;
    case '?': windowsKey = VK_OEM_2; needsShift = true; break;
    case '{': windowsKey = VK_OEM_4; needsShift = true; break;
    case '|': windowsKey = VK_OEM_5; needsShift = true; break;
    case '}': windowsKey = VK_OEM_6; needsShift = true; break;
    case '"': windowsKey = VK_OEM_7; needsShift = true; break;
    case '~': windowsKey = VK_OEM_3; needsShift = true; break;
    case ';': windowsKey = VK_OEM_1; break;
    case '=': windowsKey = VK_OEM_PLUS; break;
    case ',': windowsKey = VK_OEM_COMMA; break;
    case '-': windowsKey = VK_OEM_MINUS; break;
    case '.': windowsKey = VK_OEM_PERIOD; break;
    case '/': windowsKey = VK_OEM_2; break;
    case '[': windowsKey = VK_OEM_4; break;
    case '\\': windowsKey = VK_OEM_5; break;
    case ']': windowsKey = VK_OEM_6; break;
    case '\'': windowsKey = VK_OEM_7; break;
    case '`': windowsKey = VK_OEM_3; break;
    default:
        break;
    }
}

WebRTCManager::~WebRTCManager() {
    Cleanup();
}

void WebRTCManager::Cleanup() {

    releaseSource();

    webSocketRuns = false;

    if (webSocket && webSocket->is_open()) {

        try {

            webSocket->next_layer().cancel();

            webSocket->close(boost::beast::websocket::close_code::normal);

        }
        catch (std::exception& e) {

            Logger::getInstance()->info(e.what());

        }
    }

    if (ioContextWorkPtr) {
        ioContextWorkPtr.reset();
    }

    if (ioContextThread.joinable()) {
        ioContextThread.join();
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


