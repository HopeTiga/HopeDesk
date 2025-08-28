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
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnSignalingChange - State changed from previous to: " + std::to_string(static_cast<int>(new_state)));

    switch (new_state) {
    case webrtc::PeerConnectionInterface::kStable:
        Logger::getInstance()->info("Signaling state: kStable");
        break;
    case webrtc::PeerConnectionInterface::kHaveLocalOffer:
        Logger::getInstance()->info("Signaling state: kHaveLocalOffer");
        break;
    case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
        Logger::getInstance()->info("Signaling state: kHaveRemoteOffer");
        break;
    case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
        Logger::getInstance()->info("Signaling state: kHaveLocalPrAnswer");
        break;
    case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
        Logger::getInstance()->info("Signaling state: kHaveRemotePrAnswer");
        break;
    case webrtc::PeerConnectionInterface::kClosed:
        Logger::getInstance()->info("Signaling state: kClosed");
        break;
    default:
        Logger::getInstance()->warning("Unknown signaling state: " + std::to_string(static_cast<int>(new_state)));
        break;
    }
}

void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnDataChannel - Data channel received: " + data_channel->label());
    Logger::getInstance()->info("Data channel state: " + std::to_string(static_cast<int>(data_channel->state())));
}

void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnIceGatheringChange - ICE gathering state: " + std::to_string(static_cast<int>(new_state)));

    switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceGatheringNew:
        Logger::getInstance()->info("ICE gathering: kIceGatheringNew");
        break;
    case webrtc::PeerConnectionInterface::kIceGatheringGathering:
        Logger::getInstance()->info("ICE gathering: kIceGatheringGathering");
        break;
    case webrtc::PeerConnectionInterface::kIceGatheringComplete:
        Logger::getInstance()->info("ICE gathering: kIceGatheringComplete - All candidates gathered");
        break;
    default:
        Logger::getInstance()->warning("Unknown ICE gathering state: " + std::to_string(static_cast<int>(new_state)));
        break;
    }
}

void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnIceCandidate - New ICE candidate received");

    if (!candidate) {
        Logger::getInstance()->error("OnIceCandidate called with null candidate");
        return;
    }

    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert ICE candidate to string");
        return;
    }

    Logger::getInstance()->info("ICE candidate SDP: " + sdp.substr(0, 100) + "...");
    Logger::getInstance()->info("ICE candidate mid: " + candidate->sdp_mid());
    Logger::getInstance()->info("ICE candidate mline index: " + std::to_string(candidate->sdp_mline_index()));

    boost::json::object msg;
    msg["type"] = "candidate";
    msg["candidate"] = sdp;
    msg["mid"] = candidate->sdp_mid();
    msg["mlineIndex"] = candidate->sdp_mline_index();

    Logger::getInstance()->info("Sending ICE candidate via signaling");
    manager_->sendSignalingMessage(msg);
    Logger::getInstance()->info("ICE candidate sent successfully");
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnIceConnectionChange - ICE connection state: " + std::to_string(static_cast<int>(new_state)));

    switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
        Logger::getInstance()->info("ICE connection: kIceConnectionNew");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
        Logger::getInstance()->info("ICE connection: kIceConnectionChecking - Establishing connection");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
        Logger::getInstance()->info("ICE connection: kIceConnectionConnected - WebRTC connection established successfully!");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
        Logger::getInstance()->info("ICE connection: kIceConnectionCompleted");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
        Logger::getInstance()->error("ICE connection: kIceConnectionFailed - Connection failed, triggering cleanup");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
        Logger::getInstance()->warning("ICE connection: kIceConnectionDisconnected");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
        Logger::getInstance()->info("ICE connection: kIceConnectionClosed");
        break;
    default:
        Logger::getInstance()->warning("Unknown ICE connection state: " + std::to_string(static_cast<int>(new_state)));
        break;
    }
}

void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnConnectionChange - Peer connection state: " + std::to_string(static_cast<int>(new_state)));

    switch (new_state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
        Logger::getInstance()->info("Peer connection: kNew");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
        Logger::getInstance()->info("Peer connection: kConnecting");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
        Logger::getInstance()->info("Peer connection: kConnected - Full connection established");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
        Logger::getInstance()->warning("Peer connection: kDisconnected - Connection lost, triggering cleanup");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
        Logger::getInstance()->error("Peer connection: kFailed - Connection failed permanently, triggering cleanup");
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
        Logger::getInstance()->info("Peer connection: kClosed - Connection closed, triggering cleanup");
        break;
    default:
        Logger::getInstance()->warning("Unknown peer connection state: " + std::to_string(static_cast<int>(new_state)));
        break;
    }
}

void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer)
{
    Logger::getInstance()->info("DataChannelObserverImpl::OnMessage - Received data channel message, size: " + std::to_string(buffer.size()));

    if (buffer.size() == 0) {
        Logger::getInstance()->warning("Received empty data channel message");
        return;
    }

    if (buffer.size() > 1024 * 1024) { // 1MB limit
        Logger::getInstance()->error("Received oversized data channel message: " + std::to_string(buffer.size()) + " bytes");
        return;
    }

    const std::byte* data = reinterpret_cast<const std::byte*>(buffer.data.data());
    size_t size = buffer.size();

    Logger::getInstance()->info("Posting data channel message to IO context for processing");
    manager->ioContext.post([=]() mutable {
        Logger::getInstance()->info("Processing data channel message in IO context");
        manager->processDataChannelMessage(std::vector<std::byte>(data, data + size));
        Logger::getInstance()->info("Data channel message processing completed");
        });
}

// SetLocalDescriptionObserver实现
void SetLocalDescriptionObserver::OnSuccess() {
    Logger::getInstance()->info("SetLocalDescriptionObserver::OnSuccess - SetLocalDescription succeeded");
}

void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetLocalDescriptionObserver::OnFailure - SetLocalDescription failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

// SetRemoteDescriptionObserver实现
void SetRemoteDescriptionObserver::OnSuccess() {
    Logger::getInstance()->info("SetRemoteDescriptionObserver::OnSuccess - SetRemoteDescription succeeded");
}

void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetRemoteDescriptionObserver::OnFailure - SetRemoteDescription failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

// CreateOfferObserverImpl实现
void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    Logger::getInstance()->info("CreateOfferObserverImpl::OnSuccess - CreateOffer succeeded");

    if (!desc) {
        Logger::getInstance()->error("CreateOffer success callback received null description");
        manager_->isProcessingOffer = false;
        return;
    }

    Logger::getInstance()->info("Setting local description from created offer");
    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert offer description to string");
        manager_->isProcessingOffer = false;
        return;
    }

    Logger::getInstance()->info("Offer SDP length: " + std::to_string(sdp.length()));
    Logger::getInstance()->info("Offer SDP preview: " + sdp.substr(0, 200) + "...");

    boost::json::object msg;
    msg["type"] = "offer";
    msg["sdp"] = sdp;

    Logger::getInstance()->info("Sending offer via signaling message");
    manager_->sendSignalingMessage(msg);
    Logger::getInstance()->info("Offer sent successfully");
}

void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateOfferObserverImpl::OnFailure - CreateOffer failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));

    // Reset processing flag on failure
    Logger::getInstance()->info("Resetting offer processing flag due to failure");
    manager_->isProcessingOffer = false;
}

// CreateAnswerObserverImpl实现
void CreateAnswerObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    Logger::getInstance()->info("CreateAnswerObserverImpl::OnSuccess - CreateAnswer succeeded");

    if (!desc) {
        Logger::getInstance()->error("CreateAnswer success callback received null description");
        return;
    }

    Logger::getInstance()->info("Setting local description from created answer");
    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert answer description to string");
        return;
    }

    Logger::getInstance()->info("Answer SDP length: " + std::to_string(sdp.length()));
    Logger::getInstance()->info("Answer SDP preview: " + sdp.substr(0, 200) + "...");

    boost::json::object msg;
    msg["type"] = "answer";
    msg["sdp"] = sdp;

    Logger::getInstance()->info("Sending answer via signaling message");
    manager_->sendSignalingMessage(msg);
    Logger::getInstance()->info("Answer sent successfully");
}

void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateAnswerObserverImpl::OnFailure - CreateAnswer failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

// Add releaseSource implementation
void WebRTCManager::releaseSource() {
    Logger::getInstance()->info("WebRTCManager::releaseSource - Starting WebRTC resource cleanup");

    // Stop screen capture first
    if (screenCapture) {
        Logger::getInstance()->info("Stopping and releasing screen capture");
        screenCapture.reset();
        Logger::getInstance()->info("Screen capture released");
    }
    else {
        Logger::getInstance()->info("No screen capture to release");
    }

    // Close peer connection
    if (peerConnection) {
        Logger::getInstance()->info("Closing peer connection");
        peerConnection->Close();
        peerConnection = nullptr;
        Logger::getInstance()->info("Peer connection closed and reset");
    }
    else {
        Logger::getInstance()->info("No peer connection to close");
    }

    // Reset observers
    if (peerConnectionObserver) {
        Logger::getInstance()->info("Resetting peer connection observer");
        peerConnectionObserver.reset();
    }

    if (dataChannelObserver) {
        Logger::getInstance()->info("Resetting data channel observer");
        dataChannelObserver.reset();
    }

    if (createOfferObserver) {
        Logger::getInstance()->info("Resetting create offer observer");
        createOfferObserver = nullptr;
    }

    if (createAnswerObserver) {
        Logger::getInstance()->info("Resetting create answer observer");
        createAnswerObserver = nullptr;
    }

    // Reset tracks
    if (videoTrack) {
        Logger::getInstance()->info("Resetting video track");
        videoTrack = nullptr;
    }

    if (videoSender) {
        Logger::getInstance()->info("Resetting video sender");
        videoSender = nullptr;
    }

    if (dataChannel) {
        Logger::getInstance()->info("Resetting data channel");
        dataChannel = nullptr;
    }

    if (videoTrackSourceImpl) {
        Logger::getInstance()->info("Resetting video track source");
        videoTrackSourceImpl = nullptr;
    }

    // Reset factory
    if (peerConnectionFactory) {
        Logger::getInstance()->info("Resetting peer connection factory");
        peerConnectionFactory = nullptr;
    }

    // CRITICAL: Reset state flags
    bool wasInit = isInit.load();
    bool wasProcessingOffer = isProcessingOffer.load();

    isInit = false;
    isProcessingOffer = false;

    Logger::getInstance()->info("WebRTC resource cleanup completed - isInit: " +
        std::string(wasInit ? "true->false" : "false") +
        ", isProcessingOffer: " +
        std::string(wasProcessingOffer ? "true->false" : "false"));
}

WebRTCManager::WebRTCManager(WebRTCRemoteState state)
    : tcpSocket(std::make_unique<boost::asio::ip::tcp::socket>(ioContext)),
    accept(socketIoContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), 19998)),
    state(state),
    connetState(WebRTCConnetState::none),
    peerConnection(nullptr),
    writerChannel(socketIoContext),
    winLogon(nullptr),
    keyMouseSim(nullptr),
    inputInjector(nullptr) {

    Logger::getInstance()->info("WebRTCManager::Constructor - Starting initialization with state: " + std::to_string(static_cast<int>(state)));

    Logger::getInstance()->info("Creating IO context work guard");
    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioContext));

    Logger::getInstance()->info("Starting IO context thread");
    ioContextThread = std::move(std::thread([this]() {
        Logger::getInstance()->info("IO context thread started");
        this->ioContext.run();
        Logger::getInstance()->info("IO context thread ended");
        }));

    Logger::getInstance()->info("Creating socket IO context work guard");
    socketIoContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(socketIoContext));

    Logger::getInstance()->info("Starting socket IO context thread");
    socketIoContextThread = std::move(std::thread([this]() {
        Logger::getInstance()->info("Socket IO context thread started");
        this->socketIoContext.run();
        Logger::getInstance()->info("Socket IO context thread ended");
        }));

    Logger::getInstance()->info("Starting TCP acceptor coroutine on port 19998");
    boost::asio::co_spawn(socketIoContext, [this]()-> boost::asio::awaitable<void> {
        Logger::getInstance()->info("TCP acceptor coroutine started");
        tcpSocket = std::make_unique<boost::asio::ip::tcp::socket>(socketIoContext);
        Logger::getInstance()->info("Waiting for TCP connection on port 19998");
        co_await accept.async_accept(*tcpSocket, boost::asio::use_awaitable);
        Logger::getInstance()->info("TCP connection accepted successfully");
        socketEventLoop();
        }, [this](std::exception_ptr p) {
            try {
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const boost::system::system_error& e) {
                Logger::getInstance()->error("TCP acceptor coroutine system_error: " + std::string(e.what()));
                Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("TCP acceptor coroutine exception: " + std::string(e.what()));
            }
            catch (...) {
                Logger::getInstance()->error("TCP acceptor coroutine unknown exception");
            }
            });

        Logger::getInstance()->info("Initializing KeyMouseSimulator");
        keyMouseSim = std::make_unique<KeyMouseSimulator>();

        if (!keyMouseSim->Initialize()) {
            Logger::getInstance()->error("KeyMouseSimulator initialization failed!");
        }
        else {
            Logger::getInstance()->info("KeyMouseSimulator initialized successfully");
        }

        Logger::getInstance()->info("Creating InputInjector");
        inputInjector = winrt::Windows::UI::Input::Preview::Injection::InputInjector::TryCreate();

        if (inputInjector) {
            Logger::getInstance()->info("InputInjector created successfully");
        }
        else {
            Logger::getInstance()->warning("InputInjector creation failed, will fall back to KeyMouseSimulator");
        }

        Logger::getInstance()->info("WebRTCManager initialization completed successfully");
}

void WebRTCManager::sendSignalingMessage(const boost::json::object& message) {
    Logger::getInstance()->info("WebRTCManager::sendSignalingMessage - Preparing signaling message");

    boost::json::object fullMsg;
    fullMsg["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
    fullMsg["accountID"] = accountID;
    fullMsg["targetID"] = targetID;
    fullMsg["state"] = 200;

    for (auto& [key, value] : message) {
        fullMsg[key] = value;
        Logger::getInstance()->info("Added message field: " + std::string(key) + " = " + boost::json::serialize(value));
    }

    std::string msgStr = boost::json::serialize(fullMsg);
    Logger::getInstance()->info("Signaling message serialized, length: " + std::to_string(msgStr.size()));
    Logger::getInstance()->info("Message content: " + msgStr);

    auto data = std::make_shared<WriterData>(const_cast<char*>(msgStr.c_str()), msgStr.size());

    Logger::getInstance()->info("Queuing signaling message for async write");
    writerAsync(data);
    Logger::getInstance()->info("Signaling message queued successfully");
}

void WebRTCManager::processOffer(const std::string& sdp) {
    Logger::getInstance()->info("WebRTCManager::processOffer - Processing incoming offer, SDP length: " + std::to_string(sdp.length()));

    if (sdp.empty()) {
        Logger::getInstance()->error("Received empty SDP offer");
        isInit = false;
        return;
    }

    Logger::getInstance()->info("Offer SDP preview: " + sdp.substr(0, 200) + "...");

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

    if (desc) {
        Logger::getInstance()->info("Offer parsed successfully, setting as remote description");
        peerConnection->SetRemoteDescription(
            SetRemoteDescriptionObserver::Create().get(), desc.release()); // 使用 release()

        Logger::getInstance()->info("Creating answer for received offer");
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        createAnswerObserver = CreateAnswerObserverImpl::Create(this, peerConnection);
        peerConnection->CreateAnswer(createAnswerObserver.get(), options);
        Logger::getInstance()->info("Answer creation initiated");
    }
    else {
        Logger::getInstance()->error("Failed to parse offer SDP: " + error.description);
        Logger::getInstance()->error("Parse error line: " + error.line);
        isInit = false;
    }
}

void WebRTCManager::processAnswer(const std::string& sdp) {
    Logger::getInstance()->info("WebRTCManager::processAnswer - Processing incoming answer, SDP length: " + std::to_string(sdp.length()));

    if (sdp.empty()) {
        Logger::getInstance()->error("Received empty SDP answer");
        isInit = false;
        return;
    }

    Logger::getInstance()->info("Answer SDP preview: " + sdp.substr(0, 200) + "...");

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);

    if (desc) {
        Logger::getInstance()->info("Answer parsed successfully, setting as remote description");
        peerConnection->SetRemoteDescription(
            SetRemoteDescriptionObserver::Create().get(), desc.release()); // 使用 release()
    }
    else {
        Logger::getInstance()->error("Failed to parse answer SDP: " + error.description);
        Logger::getInstance()->error("Parse error line: " + error.line);
        isInit = false;
    }
}

void WebRTCManager::processIceCandidate(const std::string& candidate,
    const std::string& mid, int lineIndex) {
    Logger::getInstance()->info("WebRTCManager::processIceCandidate - Processing ICE candidate");
    Logger::getInstance()->info("Candidate: " + candidate.substr(0, 100) + "...");
    Logger::getInstance()->info("Mid: " + mid + ", Line index: " + std::to_string(lineIndex));

    if (candidate.empty()) {
        Logger::getInstance()->warning("Received empty ICE candidate");
        return;
    }

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
        webrtc::CreateIceCandidate(mid, lineIndex, candidate, &error));

    if (ice_candidate) {
        Logger::getInstance()->info("ICE candidate parsed successfully, adding to peer connection");
        peerConnection->AddIceCandidate(ice_candidate.release()); // 使用 release()
        Logger::getInstance()->info("ICE candidate added successfully");
    }
    else {
        Logger::getInstance()->error("Failed to parse ICE candidate: " + error.description);
        Logger::getInstance()->error("Parse error line: " + error.line);
    }
}

void WebRTCManager::socketEventLoop() {
    Logger::getInstance()->info("WebRTCManager::socketEventLoop - Starting socket event loop");
    socketRuns = true;

    // 读取协程
    boost::asio::co_spawn(socketIoContext, [this]() -> boost::asio::awaitable<void> {
        Logger::getInstance()->info("Reader coroutine started");

        try {
            char headerBuffer[8];
            size_t headerSize = sizeof(int64_t);
            int messageCount = 0;

            while (socketRuns) {
                std::memset(headerBuffer, 0, headerSize);
                ++messageCount;
                Logger::getInstance()->info("Waiting to receive message header #" + std::to_string(messageCount));

                // 接收消息头
                size_t headerRead = 0;
                while (headerRead < headerSize) {
                    try {
                        size_t n = co_await this->tcpSocket->async_read_some(
                            boost::asio::buffer(headerBuffer + headerRead, headerSize - headerRead),
                            boost::asio::use_awaitable);

                        if (n == 0) {
                            Logger::getInstance()->warning("Connection closed by peer (header read returned 0)");
                            co_return;
                        }

                        headerRead += n;
                        Logger::getInstance()->info("Read " + std::to_string(n) + " bytes of header, total: " +
                            std::to_string(headerRead) + "/" + std::to_string(headerSize));
                    }
                    catch (const boost::system::system_error& e) {
                        Logger::getInstance()->error("Error reading header: " + std::string(e.what()));
                        Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                        co_return;
                    }
                }

                int64_t rawBodyLength = 0;
                std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
                int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

                Logger::getInstance()->info("Message #" + std::to_string(messageCount) + " body size: " + std::to_string(bodyLength));

                if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) {
                    Logger::getInstance()->error("Invalid body length: " + std::to_string(bodyLength) + " for message #" + std::to_string(messageCount));
                    co_return;
                }

                size_t bodySize = static_cast<size_t>(bodyLength);

                std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]);
                if (!bodyBuffer) {
                    Logger::getInstance()->error("Failed to allocate " + std::to_string(bodySize) + " bytes for body buffer");
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
                            Logger::getInstance()->warning("Connection closed by peer (body read returned 0)");
                            co_return;
                        }

                        bodyRead += n;
                        Logger::getInstance()->info("Read " + std::to_string(n) + " bytes of body, total: " +
                            std::to_string(bodyRead) + "/" + std::to_string(bodySize));
                    }
                    catch (const boost::system::system_error& e) {
                        Logger::getInstance()->error("Error reading body: " + std::string(e.what()));
                        Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                        co_return;
                    }
                }

                std::string bodyStr(bodyBuffer.get(), bodySize);
                Logger::getInstance()->info("Received complete message #" + std::to_string(messageCount) +
                    ", body preview: " + bodyStr.substr(0, 200) + "...");

                // 解析JSON
                try {
                    Logger::getInstance()->info("Parsing JSON message");
                    boost::json::object json = boost::json::parse(bodyStr).as_object();
                    Logger::getInstance()->info("JSON parsed successfully");

                    if (json.contains("requestType")) {
                        int64_t requestType = json["requestType"].as_int64();
                        Logger::getInstance()->info("Processing request type: " + std::to_string(requestType));

                        if (!json.contains("state")) {
                            Logger::getInstance()->warning("JSON missing 'state' field");
                            continue;
                        }

                        int64_t responseState = json["state"].as_int64();
                        Logger::getInstance()->info("Response state: " + std::to_string(responseState));

                        if (WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER) {
                            Logger::getInstance()->info("Processing REGISTER request");

                            if (responseState == 200) {
                                Logger::getInstance()->info("REGISTER successful, updating connection state");
                                connetState = WebRTCConnetState::connect;

                                if (json.contains("message")) {
                                    Logger::getInstance()->info("Server message: " + std::string(json["message"].as_string().c_str()));
                                }

                                Logger::getInstance()->info("Initializing peer connection after successful registration");
                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to initialize peer connection after registration");
                                }
                            }
                            else {
                                Logger::getInstance()->error("REGISTER failed with state: " + std::to_string(responseState));
                            }
                        }
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST) {
                            Logger::getInstance()->info("Processing general REQUEST");
                            if (responseState == 200) {
                                Logger::getInstance()->info("REQUEST successful, processing content");

                                if (json.contains("webRTCRemoteState")) {
                                    WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());
                                    Logger::getInstance()->info("Remote state received: " + std::to_string(static_cast<int>(remoteState)));

                                    if (state.load() == WebRTCRemoteState::nullRemote) {
                                        Logger::getInstance()->info("Current state is nullRemote, checking remote state");

                                        if (remoteState == WebRTCRemoteState::masterRemote) {
                                            Logger::getInstance()->info("Remote is master, setting self as follower");
                                            state = WebRTCRemoteState::followerRemote;

                                            Logger::getInstance()->info("Initializing peer connection as follower");
                                            if (!initializePeerConnection()) {
                                                Logger::getInstance()->error("Failed to initialize peer connection as follower");
                                                continue;
                                            }

                                            Logger::getInstance()->info("Initializing screen capture as follower");
                                            if (!initializeScreenCapture()) {
                                                Logger::getInstance()->error("Failed to initialize screen capture as follower");
                                                continue;
                                            }

                                            if (json.contains("accountID")) {
                                                targetID = std::string(json["accountID"].as_string().c_str());
                                                Logger::getInstance()->info("Target ID set to: " + targetID);
                                            }

                                            if (json.contains("targetID")) {
                                                accountID = std::string(json["targetID"].as_string().c_str());
                                                Logger::getInstance()->info("Account ID set to: " + accountID);
                                            }

                                            // Check if we're not already processing an offer
                                            if (!isProcessingOffer.exchange(true)) {
                                                Logger::getInstance()->info("Creating and sending offer to establish connection");

                                                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                                                options.offer_to_receive_video = true;
                                                options.offer_to_receive_audio = false;

                                                createOfferObserver = CreateOfferObserverImpl::Create(this, peerConnection);
                                                peerConnection->CreateOffer(createOfferObserver.get(), options);
                                                Logger::getInstance()->info("Offer creation initiated");
                                            }
                                            else {
                                                Logger::getInstance()->warning("Already processing an offer, skipping new offer creation");
                                            }
                                        }
                                    }
                                    else {
                                        Logger::getInstance()->info("State already set to: " +
                                            std::to_string(static_cast<int>(state.load())) + ", ignoring remote state");
                                    }
                                }

                                if (json.contains("type")) {
                                    std::string type(json["type"].as_string().c_str());
                                    Logger::getInstance()->info("Processing WebRTC message type: " + type);

                                    if (type == "answer") {
                                        Logger::getInstance()->info("Processing incoming answer");

                                        if (!isInit.load()) {
                                            Logger::getInstance()->info("Processing answer to complete connection setup");

                                            std::string sdp(json["sdp"].as_string().c_str());
                                            processAnswer(sdp);

                                            Logger::getInstance()->info("Answer processed, connection setup complete");
                                            isInit = true;
                                            isProcessingOffer = false;
                                        }
                                        else {
                                            Logger::getInstance()->warning("Connection already initialized, ignoring duplicate answer");
                                        }
                                    }
                                    else if (type == "candidate") {
                                        Logger::getInstance()->info("Processing incoming ICE candidate");

                                        std::string candidateStr(json["candidate"].as_string().c_str());
                                        std::string mid = json.contains("mid") ? std::string(json["mid"].as_string().c_str()) : "";
                                        int lineIndex = json.contains("mlineIndex") ? json["mlineIndex"].as_int64() : 0;

                                        Logger::getInstance()->info("ICE candidate mid: " + mid + ", line index: " + std::to_string(lineIndex));

                                        if (peerConnection) {
                                            processIceCandidate(candidateStr, mid, lineIndex);
                                        }
                                        else {
                                            Logger::getInstance()->warning("No peer connection available, ignoring ICE candidate");
                                        }
                                    }
                                }
                            }
                            else {
                                Logger::getInstance()->error("General REQUEST failed with state: " + std::to_string(responseState));
                            }
                        }
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::RESTART) {
                            Logger::getInstance()->info("Processing RESTART request");

                            if (responseState == 200) {
                                Logger::getInstance()->info("RESTART successful, cleaning up existing resources");
                                releaseSource();

                                Logger::getInstance()->info("Resetting state to nullRemote");
                                state = WebRTCRemoteState::nullRemote;

                                Logger::getInstance()->info("Reinitializing after restart");
                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to reinitialize PeerConnection after restart");
                                }
                            }
                            else {
                                Logger::getInstance()->error("RESTART failed with state: " + std::to_string(responseState));
                            }
                        }
                        else if (WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE) {
                            Logger::getInstance()->info("Processing STOPREMOTE request");

                            if (responseState == 200) {
                                Logger::getInstance()->info("STOPREMOTE successful, cleaning up resources");
                                releaseSource();

                                Logger::getInstance()->info("Resetting state to nullRemote after stop");
                                state = WebRTCRemoteState::nullRemote;

                                Logger::getInstance()->info("Reinitializing for new connections after stop");
                                if (!initializePeerConnection()) {
                                    Logger::getInstance()->error("Failed to reinitialize PeerConnection after stop");
                                }
                            }
                            else {
                                Logger::getInstance()->error("STOPREMOTE failed with state: " + std::to_string(responseState));
                            }
                        }
                        else {
                            Logger::getInstance()->warning("Unknown request type: " + std::to_string(requestType));
                        }
                    }
                    else {
                        Logger::getInstance()->warning("JSON missing 'requestType' field");
                    }
                }
                catch (const boost::system::system_error& e) {
                    Logger::getInstance()->error("System error during JSON processing: " + std::string(e.what()));
                    Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                }
                catch (const std::exception& e) {
                    Logger::getInstance()->error("Exception processing message #" + std::to_string(messageCount) + ": " + std::string(e.what()));
                }
            }
        }
        catch (const std::exception& e) {
            Logger::getInstance()->error("Reader coroutine fatal exception: " + std::string(e.what()));
        }
        catch (...) {
            Logger::getInstance()->error("Reader coroutine unknown fatal exception");
        }

        Logger::getInstance()->info("Reader coroutine ended");
        co_return;

        }, [this](std::exception_ptr p) {
            try {
                if (p) {
                    std::rethrow_exception(p);
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("Reader coroutine completion handler exception: " + std::string(e.what()));
            }
            });

        // 写入协程
        boost::asio::co_spawn(socketIoContext, [this]() -> boost::asio::awaitable<void> {
            Logger::getInstance()->info("Writer coroutine started");

            try {
                int writeCount = 0;

                for (;;) {
                    while (this->writerDataQueues.size_approx() == 0 && this->socketRuns.load()) {
                        Logger::getInstance()->info("Writer waiting for data, queue empty");
                        co_await this->writerChannel.async_receive(boost::asio::use_awaitable);
                        Logger::getInstance()->info("Writer received wake-up signal");
                    }

                    if (!socketRuns) {
                        Logger::getInstance()->info("Socket runs flag is false, processing remaining queue items");

                        size_t remainingItems = this->writerDataQueues.size_approx();
                        Logger::getInstance()->info("Processing " + std::to_string(remainingItems) + " remaining queue items during shutdown");

                        while (this->writerDataQueues.size_approx() > 0) {
                            std::shared_ptr<WriterData> nowNode = nullptr;
                            if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                                try {
                                    Logger::getInstance()->info("Sending remaining data during shutdown, size: " + std::to_string(nowNode->size));
                                    co_await boost::asio::async_write(*this->tcpSocket,
                                        boost::asio::buffer(nowNode->data, nowNode->size),
                                        boost::asio::use_awaitable);
                                    Logger::getInstance()->info("Remaining data sent successfully");
                                }
                                catch (const std::exception& e) {
                                    Logger::getInstance()->error("Error sending remaining data during shutdown: " + std::string(e.what()));
                                    break;
                                }
                            }
                        }
                        Logger::getInstance()->info("Writer coroutine shutdown complete");
                        co_return;
                    }

                    std::shared_ptr<WriterData> nowNode = nullptr;
                    if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                        try {
                            ++writeCount;
                            Logger::getInstance()->info("Sending data #" + std::to_string(writeCount) +
                                ", size: " + std::to_string(nowNode->size));

                            co_await boost::asio::async_write(*this->tcpSocket,
                                boost::asio::buffer(nowNode->data, nowNode->size),
                                boost::asio::use_awaitable);

                            Logger::getInstance()->info("Data #" + std::to_string(writeCount) + " sent successfully");
                        }
                        catch (const boost::system::system_error& e) {
                            Logger::getInstance()->error("System error sending data #" + std::to_string(writeCount) +
                                ": " + std::string(e.what()));
                            Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                            break;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                Logger::getInstance()->error("Writer coroutine fatal exception: " + std::string(e.what()));
            }

            Logger::getInstance()->info("Writer coroutine ended");
            co_return;

            }, [this](std::exception_ptr p) {
                try {
                    if (p) {
                        std::rethrow_exception(p);
                    }
                }
                catch (const std::exception& e) {
                    Logger::getInstance()->error("Writer coroutine completion handler exception: " + std::string(e.what()));
                }
                });

            Logger::getInstance()->info("Socket event loop setup complete - reader and writer coroutines started");
}

void WebRTCManager::writerAsync(std::shared_ptr<WriterData> data) {
    if (!data) {
        Logger::getInstance()->error("WebRTCManager::writerAsync - Null data provided");
        return;
    }

    Logger::getInstance()->info("WebRTCManager::writerAsync - Queuing data for async write, size: " + std::to_string(data->size));

    writerDataQueues.enqueue(data);
    size_t queueSize = writerDataQueues.size_approx();
    Logger::getInstance()->info("Data queued, approximate queue size: " + std::to_string(queueSize));

    if (writerChannel.is_open()) {
        bool signalSent = writerChannel.try_send(boost::system::error_code{});
        Logger::getInstance()->info("Writer channel signal sent: " + std::string(signalSent ? "success" : "failed"));
    }
    else {
        Logger::getInstance()->warning("Writer channel is not open, signal not sent");
    }
}

bool WebRTCManager::initializePeerConnection() {
    Logger::getInstance()->info("WebRTCManager::initializePeerConnection - Starting peer connection initialization");

    // Clean up any existing connection first
    if (peerConnection) {
        Logger::getInstance()->warning("Existing peer connection found, cleaning up before reinitializing");
        releaseSource();
    }

    Logger::getInstance()->info("Initializing WebRTC SSL");
    webrtc::InitializeSSL();

    Logger::getInstance()->info("Creating WebRTC threads");

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
    Logger::getInstance()->info("Network thread created and started");

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
    Logger::getInstance()->info("Worker thread created and started");

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
    Logger::getInstance()->info("Signaling thread created and started");

    Logger::getInstance()->info("Creating PeerConnectionFactory");
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
    Logger::getInstance()->info("PeerConnectionFactory created successfully");

    Logger::getInstance()->info("Configuring PeerConnection settings");
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

    Logger::getInstance()->info("Adding STUN server");
    webrtc::PeerConnectionInterface::IceServer stun_server;
    stun_server.uri = "stun:14.103.170.36:3478";
    config.servers.push_back(stun_server);

    Logger::getInstance()->info("Adding TURN server");
    webrtc::PeerConnectionInterface::IceServer turn_server;
    turn_server.uri = "turn:14.103.170.36:3478";
    turn_server.username = "HopeTiga";
    turn_server.password = "dy913140924";
    config.servers.emplace_back(turn_server);


    Logger::getInstance()->info("Creating PeerConnection observer");
    peerConnectionObserver = std::make_unique<PeerConnectionObserverImpl>(this);

    Logger::getInstance()->info("Creating PeerConnection");
    webrtc::PeerConnectionDependencies pc_dependencies(peerConnectionObserver.get());

    auto pcResult = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pc_dependencies));
    if (!pcResult.ok()) {
        Logger::getInstance()->error("Failed to create PeerConnection: " + std::string(pcResult.error().message()));
        return false;
    }

    peerConnection = pcResult.MoveValue();
    Logger::getInstance()->info("PeerConnection created successfully");

    // 如果是发送端，创建视频源和轨道
    if (state == WebRTCRemoteState::followerRemote) {
        Logger::getInstance()->info("Configuring as video sender (follower remote)");

        Logger::getInstance()->info("Creating video track source");
        videoTrackSourceImpl = webrtc::make_ref_counted<VideoTrackSourceImpl>();

        Logger::getInstance()->info("Creating video track");
        videoTrack = peerConnectionFactory->CreateVideoTrack(videoTrackSourceImpl, "videoTrack");

        if (!videoTrack) {
            Logger::getInstance()->error("Failed to create video track");
            return false;
        }
        Logger::getInstance()->info("Video track created successfully");

        Logger::getInstance()->info("Configuring RTP encoding parameters");
        std::vector<webrtc::RtpEncodingParameters> encodings;
        webrtc::RtpEncodingParameters encoding;

        encoding.active = true;
        encoding.max_bitrate_bps = 4000000;  // 4 Mbps
        encoding.min_bitrate_bps = 500000;   // 500 kbps
        encoding.max_framerate = 60;
        encoding.scale_resolution_down_by = 1.0;
        encoding.scalability_mode = "L1T1";

        encodings.push_back(encoding);
        Logger::getInstance()->info("RTP encoding parameters configured - Max bitrate: " +
            std::to_string(encoding.max_bitrate_bps.value()) + " bps, Max framerate: " +
            std::to_string(encoding.max_framerate.value()));

        Logger::getInstance()->info("Adding video track to PeerConnection");
        std::vector<std::string> stream_ids = { "mediaStream" };
        auto addTrackResult = peerConnection->AddTrack(videoTrack, stream_ids, encodings);

        if (!addTrackResult.ok()) {
            Logger::getInstance()->error("Failed to add video track: " + std::string(addTrackResult.error().message()));
            return false;
        }

        videoSender = addTrackResult.MoveValue();
        Logger::getInstance()->info("Video track added to PeerConnection successfully");

        Logger::getInstance()->info("Configuring H264 codec preferences");
        auto transceivers = peerConnection->GetTransceivers();
        Logger::getInstance()->info("Found " + std::to_string(transceivers.size()) + " transceivers");

        for (auto& transceiver : transceivers) {
            if (transceiver->media_type() == webrtc::MediaType::MEDIA_TYPE_VIDEO) {
                Logger::getInstance()->info("Processing video transceiver");
                auto codecs = transceiver->codec_preferences();
                Logger::getInstance()->info("Available codecs: " + std::to_string(codecs.size()));

                std::vector<webrtc::RtpCodecCapability> preferred_codecs;
                webrtc::RtpCodecCapability h264_codec;
                bool h264_found = false;

                for (const auto& codec : codecs) {
                    Logger::getInstance()->info("Found codec: " + codec.name);
                    if (codec.name == "H264") {
                        h264_codec = codec;
                        h264_codec.parameters["profile-level-id"] = "42e01f";
                        h264_codec.parameters["level-asymmetry-allowed"] = "1";
                        h264_codec.parameters["packetization-mode"] = "1";
                        preferred_codecs.clear();
                        preferred_codecs.insert(preferred_codecs.begin(), h264_codec);
                        h264_found = true;
                        Logger::getInstance()->info("H264 codec configured with profile-level-id: 42e01f");
                    }
                    else if (codec.name != "red" && codec.name != "ulpfec") {
                        preferred_codecs.push_back(codec);
                    }
                }

                if (h264_found) {
                    Logger::getInstance()->info("Setting H264 as preferred codec");
                    auto result = transceiver->SetCodecPreferences(preferred_codecs);
                    if (!result.ok()) {
                        Logger::getInstance()->error("Failed to set codec preferences: " + std::string(result.message()));
                        return false;
                    }
                    Logger::getInstance()->info("H264 codec preferences set successfully");
                }
                else {
                    Logger::getInstance()->warning("H264 codec not found in available codecs");
                }
            }
        }

        Logger::getInstance()->info("Configuring RTP sender parameters");
        webrtc::RtpParameters parameters = videoSender->GetParameters();

        if (!parameters.encodings.empty()) {
            parameters.encodings[0] = encoding;
            Logger::getInstance()->info("Updated existing encoding parameters");
        }
        else {
            parameters.encodings = encodings;
            Logger::getInstance()->info("Set new encoding parameters");
        }

        parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;

        auto setParamsResult = videoSender->SetParameters(parameters);

        if (!setParamsResult.ok()) {
            Logger::getInstance()->error("Failed to set RTP parameters: " + std::string(setParamsResult.message()));
            return false;
        }
        Logger::getInstance()->info("RTP sender parameters configured successfully");
    }
    else {
        Logger::getInstance()->info("Configured as receiver (not follower remote)");
    }

    Logger::getInstance()->info("Creating data channel");
    std::unique_ptr<webrtc::DataChannelInit> dataChannelConfig = std::make_unique<webrtc::DataChannelInit>();

    dataChannel = peerConnection->CreateDataChannel("dataChannel", dataChannelConfig.get());
    if (!dataChannel) {
        Logger::getInstance()->error("Failed to create data channel");
        return false;
    }

    Logger::getInstance()->info("Creating data channel observer");
    dataChannelObserver = std::make_unique<DataChannelObserverImpl>(this);

    Logger::getInstance()->info("Registering data channel observer");
    dataChannel->RegisterObserver(dataChannelObserver.get());

    Logger::getInstance()->info("PeerConnection initialized successfully with H264 configuration");
    Logger::getInstance()->info("Current state - Remote: " + std::to_string(static_cast<int>(state.load())) +
        ", Connection: " + std::to_string(static_cast<int>(connetState.load())));

    return true;
}

bool WebRTCManager::initializeScreenCapture() {
    Logger::getInstance()->info("WebRTCManager::initializeScreenCapture - Starting screen capture initialization");

    if (screenCapture) {
        Logger::getInstance()->warning("Screen capture already exists, skipping initialization");
        return false;
    }

    Logger::getInstance()->info("Creating screen capture instance");
    screenCapture = std::make_shared<ScreenCapture>();

    Logger::getInstance()->info("Setting up frame callback for screen capture");
    screenCapture->setFrameCallback([this](const uint8_t* data, size_t size, int width, int height) {

        if (!videoTrackSourceImpl || !data || size == 0) {
            return;
        }

        // YUV420格式：Y占width*height，U和V各占width*height/4
        size_t expectedSize = width * height * 3 / 2;
        if (size != expectedSize) {
            Logger::getInstance()->error("Invalid YUV data size - Expected: " + std::to_string(expectedSize) +
                ", Received: " + std::to_string(size));
            return;
        }

        // 创建WebRTC的I420Buffer（YUV420格式）
        webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer =
            webrtc::I420Buffer::Create(width, height);

        if (!i420Buffer) {
            Logger::getInstance()->error("Failed to create I420Buffer");
            return;
        }

        // 计算各平面的大小和指针
        const int ySize = width * height;
        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;
        const int uvSize = uvWidth * uvHeight;

        // 直接拷贝三个平面的数据
        memcpy(i420Buffer->MutableDataY(), data, ySize);
        memcpy(i420Buffer->MutableDataU(), data + ySize, uvSize);
        memcpy(i420Buffer->MutableDataV(), data + ySize + uvSize, uvSize);

        // 创建时间戳（微秒）
        int64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 组装成VideoFrame
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420Buffer)
            .set_timestamp_us(timestamp_us)
            .build();

        // 推送给WebRTC
        videoTrackSourceImpl->PushFrame(frame);


        });

    Logger::getInstance()->info("Initializing screen capture");
    if (!screenCapture->initialize()) {
        Logger::getInstance()->error("Failed to initialize screen capture");
        return false;
    }
    Logger::getInstance()->info("Screen capture initialized successfully");

    Logger::getInstance()->info("Starting screen capture");
    if (!screenCapture->startCapture()) {
        Logger::getInstance()->error("Failed to start screen capture");
        return false;
    }
    Logger::getInstance()->info("Screen capture started successfully");

    return true;
}

void WebRTCManager::processDataChannelMessage(const std::vector<std::byte>& bytes)
{
    Logger::getInstance()->info("WebRTCManager::processDataChannelMessage - Processing data channel message, size: " + std::to_string(bytes.size()));

    if (bytes.empty()) {
        Logger::getInstance()->warning("Received empty data channel message");
        return;
    }

    if (bytes.size() < sizeof(short)) {
        Logger::getInstance()->error("Data channel message too small, size: " + std::to_string(bytes.size()));
        return;
    }

    if (!inputInjector) {
        Logger::getInstance()->warning("InputInjector is null, cannot process input events - falling back to KeyMouseSimulator");
    }

    const unsigned char* buffer = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t size = bytes.size();

    short eventType = 0;
    std::memcpy(&eventType, buffer, sizeof(short));
    Logger::getInstance()->info("Processing input event type: " + std::to_string(eventType));

    switch (eventType) {
    case 0: { // Mouse move
        Logger::getInstance()->info("Processing mouse move event");

        if (size < sizeof(short) + 2 * sizeof(int)) {
            Logger::getInstance()->error("Invalid mouse move message size: " + std::to_string(size));
            return;
        }

        int posX = 0;
        int posY = 0;

        std::memcpy(&posX, buffer + sizeof(short), sizeof(int));
        std::memcpy(&posY, buffer + sizeof(short) + sizeof(int), sizeof(int));

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        Logger::getInstance()->info("Mouse move - Raw position: (" + std::to_string(posX) + ", " + std::to_string(posY) +
            "), Screen: " + std::to_string(screenWidth) + "x" + std::to_string(screenHeight));

        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        int normalizedX = (posX * 65535) / screenWidth;
        int normalizedY = (posY * 65535) / screenHeight;

        Logger::getInstance()->info("Mouse move - Normalized position: (" + std::to_string(normalizedX) + ", " + std::to_string(normalizedY) + ")");

        try {
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo mouseInfo;
            mouseInfo.MouseOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Move |
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Absolute);
            mouseInfo.MouseData(0);
            mouseInfo.DeltaX(normalizedX);
            mouseInfo.DeltaY(normalizedY);
            mouseInfo.TimeOffsetInMilliseconds(0);

            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> mouseInputs;
            mouseInputs.push_back(mouseInfo);
            inputInjector.InjectMouseInput(mouseInputs);
            Logger::getInstance()->info("Mouse move injected successfully via InputInjector");
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for mouse move, using KeyMouseSimulator fallback");
            if (!keyMouseSim->MouseMove(posX, posY, true)) {
                Logger::getInstance()->error("Failed to move mouse via KeyMouseSimulator");
            }
        }
        break;
    }

    case 1: { // Mouse button down
        Logger::getInstance()->info("Processing mouse button down event");

        if (size < sizeof(short) * 2 + 2 * sizeof(int)) {
            Logger::getInstance()->error("Invalid mouse button down message size: " + std::to_string(size));
            return;
        }

        short mouseType = 0;
        int posX = 0;
        int posY = 0;

        std::memcpy(&mouseType, buffer + sizeof(short), sizeof(short));
        std::memcpy(&posX, buffer + sizeof(short) * 2, sizeof(int));
        std::memcpy(&posY, buffer + sizeof(short) * 2 + sizeof(int), sizeof(int));

        Logger::getInstance()->info("Mouse button down - Type: " + std::to_string(mouseType) +
            ", Position: (" + std::to_string(posX) + ", " + std::to_string(posY) + ")");

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        int normalizedX = (posX * 65535) / screenWidth;
        int normalizedY = (posY * 65535) / screenHeight;

        try {
            // 先移动鼠标到目标位置
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo moveInfo;
            moveInfo.MouseOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Move |
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Absolute);
            moveInfo.MouseData(0);
            moveInfo.DeltaX(normalizedX);
            moveInfo.DeltaY(normalizedY);
            moveInfo.TimeOffsetInMilliseconds(0);

            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> moveInputs;
            moveInputs.push_back(moveInfo);
            inputInjector.InjectMouseInput(moveInputs);

            // 按下鼠标按钮
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo buttonInfo;
            buttonInfo.MouseData(0);
            buttonInfo.DeltaX(0);
            buttonInfo.DeltaY(0);
            buttonInfo.TimeOffsetInMilliseconds(0);

            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions options;
            switch (mouseType) {
            case 0: // 左键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::LeftDown;
                Logger::getInstance()->info("Left mouse button down");
                break;
            case 1: // 右键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::RightDown;
                Logger::getInstance()->info("Right mouse button down");
                break;
            case 2: // 中键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::MiddleDown;
                Logger::getInstance()->info("Middle mouse button down");
                break;
            default:
                Logger::getInstance()->error("Unknown mouse button type: " + std::to_string(mouseType));
                return;
            }

            buttonInfo.MouseOptions(options);
            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> buttonInputs;
            buttonInputs.push_back(buttonInfo);
            inputInjector.InjectMouseInput(buttonInputs);
            Logger::getInstance()->info("Mouse button down injected successfully via InputInjector");
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for mouse button down, using KeyMouseSimulator fallback");
            if (!keyMouseSim->MouseButtonDown(mouseType, posX, posY)) {
                Logger::getInstance()->error("Failed to send mouse button down via KeyMouseSimulator");
            }
        }
        break;
    }

    case 2: { // Mouse button up
        Logger::getInstance()->info("Processing mouse button up event");

        if (size < sizeof(short) * 2 + 2 * sizeof(int)) {
            Logger::getInstance()->error("Invalid mouse button up message size: " + std::to_string(size));
            return;
        }

        short mouseType = 0;
        int posX = 0;
        int posY = 0;

        std::memcpy(&mouseType, buffer + sizeof(short), sizeof(short));
        std::memcpy(&posX, buffer + sizeof(short) * 2, sizeof(int));
        std::memcpy(&posY, buffer + sizeof(short) * 2 + sizeof(int), sizeof(int));

        Logger::getInstance()->info("Mouse button up - Type: " + std::to_string(mouseType) +
            ", Position: (" + std::to_string(posX) + ", " + std::to_string(posY) + ")");

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        posX = std::max(0, std::min(posX, screenWidth - 1));
        posY = std::max(0, std::min(posY, screenHeight - 1));

        int normalizedX = (posX * 65535) / screenWidth;
        int normalizedY = (posY * 65535) / screenHeight;

        try {
            // 先移动鼠标到目标位置
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo moveInfo;
            moveInfo.MouseOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Move |
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Absolute);
            moveInfo.MouseData(0);
            moveInfo.DeltaX(normalizedX);
            moveInfo.DeltaY(normalizedY);
            moveInfo.TimeOffsetInMilliseconds(0);

            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> moveInputs;
            moveInputs.push_back(moveInfo);
            inputInjector.InjectMouseInput(moveInputs);

            // 释放鼠标按钮
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo buttonInfo;
            buttonInfo.MouseData(0);
            buttonInfo.DeltaX(0);
            buttonInfo.DeltaY(0);
            buttonInfo.TimeOffsetInMilliseconds(0);

            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions options;
            switch (mouseType) {
            case 0: // 左键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::LeftUp;
                Logger::getInstance()->info("Left mouse button up");
                break;
            case 1: // 右键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::RightUp;
                Logger::getInstance()->info("Right mouse button up");
                break;
            case 2: // 中键
                options = winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::MiddleUp;
                Logger::getInstance()->info("Middle mouse button up");
                break;
            default:
                Logger::getInstance()->error("Unknown mouse button type: " + std::to_string(mouseType));
                return;
            }

            buttonInfo.MouseOptions(options);
            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> buttonInputs;
            buttonInputs.push_back(buttonInfo);
            inputInjector.InjectMouseInput(buttonInputs);
            Logger::getInstance()->info("Mouse button up injected successfully via InputInjector");
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for mouse button up, using KeyMouseSimulator fallback");
            if (!keyMouseSim->MouseButtonUp(mouseType)) {
                Logger::getInstance()->error("Failed to send mouse button up via KeyMouseSimulator");
            }
        }
        break;
    }

    case 3: { // Key down
        Logger::getInstance()->info("Processing key down event");

        if (size < sizeof(short) + 2 * sizeof(char)) {
            Logger::getInstance()->error("Invalid key down message size: " + std::to_string(size));
            return;
        }

        unsigned char windowsKey = 0;
        char modifiers = 0;

        std::memcpy(&windowsKey, buffer + sizeof(short), sizeof(char));
        std::memcpy(&modifiers, buffer + sizeof(short) + sizeof(char), sizeof(char));

        try {
            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo> keyInputs;

            // 检查是否需要特殊处理
            bool needsShift = false;
            unsigned char actualKey = windowsKey;

            // 处理需要Shift的符号键
            handleSymbolKeyMapping(actualKey, needsShift);

            // 如果符号键需要Shift，确保设置Shift修饰符
            if (needsShift) {
                modifiers |= 0x01;
                Logger::getInstance()->info("Symbol key requires Shift, updated modifiers to: 0x" + std::to_string(static_cast<unsigned char>(modifiers)));
            }

            // 按下修饰键（按顺序：Ctrl -> Alt -> Shift）
            if (modifiers & 0x02) { // Ctrl
                Logger::getInstance()->info("Pressing Ctrl modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo ctrlKey;
                ctrlKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Control));
                ctrlKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::None);
                keyInputs.push_back(ctrlKey);
            }
            if (modifiers & 0x04) { // Alt
                Logger::getInstance()->info("Pressing Alt modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo altKey;
                altKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Menu));
                altKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::None);
                keyInputs.push_back(altKey);
            }
            if (modifiers & 0x01) { // Shift
                Logger::getInstance()->info("Pressing Shift modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo shiftKey;
                shiftKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Shift));
                shiftKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::None);
                keyInputs.push_back(shiftKey);
            }

            // 按下主键
            Logger::getInstance()->info("Pressing main key: 0x" + std::to_string(actualKey));
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo mainKey;
            mainKey.VirtualKey(static_cast<uint16_t>(actualKey));
            mainKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::None);
            keyInputs.push_back(mainKey);

            if (!keyInputs.empty()) {
                inputInjector.InjectKeyboardInput(keyInputs);
                Logger::getInstance()->info("Key down sequence injected successfully via InputInjector (" +
                    std::to_string(keyInputs.size()) + " keys)");
            }
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for key down, using KeyMouseSimulator fallback");
            if (!keyMouseSim->KeyDown(windowsKey, modifiers)) {
                Logger::getInstance()->error("Failed to send key down via KeyMouseSimulator");
            }
        }
        break;
    }

    case 4: { // Key up
        Logger::getInstance()->info("Processing key up event");

        if (size < sizeof(short) + 2 * sizeof(char)) {
            Logger::getInstance()->error("Invalid key up message size: " + std::to_string(size));
            return;
        }

        unsigned char windowsKey = 0;
        char modifiers = 0;

        std::memcpy(&windowsKey, buffer + sizeof(short), sizeof(char));
        std::memcpy(&modifiers, buffer + sizeof(short) + sizeof(char), sizeof(char));

        Logger::getInstance()->info("Key up - VK=0x" + std::to_string(windowsKey) +
            ", modifiers=0x" + std::to_string(static_cast<unsigned char>(modifiers)));

        try {
            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo> keyInputs;

            // 处理符号键映射
            bool needsShift = false;
            unsigned char actualKey = windowsKey;
            handleSymbolKeyMapping(actualKey, needsShift);

            if (needsShift) {
                modifiers |= 0x01;
                Logger::getInstance()->info("Symbol key requires Shift, updated modifiers for key up");
            }

            // 释放主键
            Logger::getInstance()->info("Releasing main key: 0x" + std::to_string(actualKey));
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo mainKey;
            mainKey.VirtualKey(static_cast<uint16_t>(actualKey));
            mainKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::KeyUp);
            keyInputs.push_back(mainKey);

            // 释放修饰键（逆序：Shift -> Alt -> Ctrl）
            if (modifiers & 0x01) { // Shift
                Logger::getInstance()->info("Releasing Shift modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo shiftKey;
                shiftKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Shift));
                shiftKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::KeyUp);
                keyInputs.push_back(shiftKey);
            }
            if (modifiers & 0x04) { // Alt
                Logger::getInstance()->info("Releasing Alt modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo altKey;
                altKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Menu));
                altKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::KeyUp);
                keyInputs.push_back(altKey);
            }
            if (modifiers & 0x02) { // Ctrl
                Logger::getInstance()->info("Releasing Ctrl modifier");
                winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyboardInfo ctrlKey;
                ctrlKey.VirtualKey(static_cast<uint16_t>(winrt::Windows::System::VirtualKey::Control));
                ctrlKey.KeyOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputKeyOptions::KeyUp);
                keyInputs.push_back(ctrlKey);
            }

            if (!keyInputs.empty()) {
                inputInjector.InjectKeyboardInput(keyInputs);
                Logger::getInstance()->info("Key up sequence injected successfully via InputInjector (" +
                    std::to_string(keyInputs.size()) + " keys)");
            }
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for key up, using KeyMouseSimulator fallback");
            if (!keyMouseSim->KeyUp(windowsKey, modifiers)) {
                Logger::getInstance()->error("Failed to send key up via KeyMouseSimulator");
            }
        }
        break;
    }

    case 5: { // Mouse wheel
        Logger::getInstance()->info("Processing mouse wheel event");

        if (size < sizeof(short) + sizeof(int)) {
            Logger::getInstance()->error("Invalid mouse wheel message size: " + std::to_string(size));
            return;
        }

        int wheelValue = 0;
        std::memcpy(&wheelValue, buffer + sizeof(short), sizeof(int));

        Logger::getInstance()->info("Mouse wheel - Value: " + std::to_string(wheelValue));

        try {
            winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo mouseInfo;
            mouseInfo.MouseOptions(winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseOptions::Wheel);
            mouseInfo.MouseData(wheelValue);
            mouseInfo.DeltaX(0);
            mouseInfo.DeltaY(0);
            mouseInfo.TimeOffsetInMilliseconds(0);

            std::vector<winrt::Windows::UI::Input::Preview::Injection::InjectedInputMouseInfo> mouseInputs;
            mouseInputs.push_back(mouseInfo);
            inputInjector.InjectMouseInput(mouseInputs);
            Logger::getInstance()->info("Mouse wheel injected successfully via InputInjector");
        }
        catch (...) {
            Logger::getInstance()->info("InputInjector failed for mouse wheel, using KeyMouseSimulator fallback");
            if (!keyMouseSim->MouseWheel(wheelValue)) {
                Logger::getInstance()->error("Failed to send mouse wheel via KeyMouseSimulator");
            }
        }
        break;
    }

    default:
        Logger::getInstance()->warning("Unknown input event type: " + std::to_string(eventType));
        break;
    }
}

void WebRTCManager::handleSymbolKeyMapping(unsigned char& windowsKey, bool& needsShift)
{
    Logger::getInstance()->info("WebRTCManager::handleSymbolKeyMapping - Processing key: 0x" + std::to_string(windowsKey));

    needsShift = false;

    // 特殊处理：如果是OEM键且需要Shift的情况
    if (windowsKey >= VK_OEM_1 && windowsKey <= VK_OEM_8) {
        Logger::getInstance()->info("OEM key detected: 0x" + std::to_string(windowsKey));
        return;
    }

    // 如果是方向键等特殊键，直接返回
    if (windowsKey >= VK_LEFT && windowsKey <= VK_DOWN) {
        Logger::getInstance()->info("Arrow key detected: 0x" + std::to_string(windowsKey));
        return;
    }

    unsigned char originalKey = windowsKey;

    // 处理ASCII字符到VK码的映射
    switch (windowsKey) {
        // 数字行的符号键（需要Shift）
    case '!': windowsKey = 0x31; needsShift = true; break; // Shift+1
    case '@': windowsKey = 0x32; needsShift = true; break; // Shift+2
    case '#': windowsKey = 0x33; needsShift = true; break; // Shift+3
    case '$': windowsKey = 0x34; needsShift = true; break; // Shift+4
    case '%': windowsKey = 0x35; needsShift = true; break; // Shift+5
    case '^': windowsKey = 0x36; needsShift = true; break; // Shift+6
    case '&': windowsKey = 0x37; needsShift = true; break; // Shift+7
    case '*': windowsKey = 0x38; needsShift = true; break; // Shift+8
    case '(': windowsKey = 0x39; needsShift = true; break; // Shift+9
    case ')': windowsKey = 0x30; needsShift = true; break; // Shift+0

        // 其他需要Shift的符号键
    case ':': windowsKey = VK_OEM_1; needsShift = true; break;     // Shift+; → :
    case '+': windowsKey = VK_OEM_PLUS; needsShift = true; break;  // Shift+= → +
    case '<': windowsKey = VK_OEM_COMMA; needsShift = true; break; // Shift+, → 
    case '_': windowsKey = VK_OEM_MINUS; needsShift = true; break; // Shift+- → _
    case '>': windowsKey = VK_OEM_PERIOD; needsShift = true; break;// Shift+. → >
    case '?': windowsKey = VK_OEM_2; needsShift = true; break;     // Shift+/ → ?
    case '{': windowsKey = VK_OEM_4; needsShift = true; break;     // Shift+[ → {
    case '|': windowsKey = VK_OEM_5; needsShift = true; break;     // Shift+\ → |
    case '}': windowsKey = VK_OEM_6; needsShift = true; break;     // Shift+] → }
    case '"': windowsKey = VK_OEM_7; needsShift = true; break;     // Shift+' → "
    case '~': windowsKey = VK_OEM_3; needsShift = true; break;     // Shift+` → ~

        // 基础符号键（不需要Shift）
    case ';': windowsKey = VK_OEM_1; break;      // ;
    case '=': windowsKey = VK_OEM_PLUS; break;   // =
    case ',': windowsKey = VK_OEM_COMMA; break;  // ,
    case '-': windowsKey = VK_OEM_MINUS; break;  // -
    case '.': windowsKey = VK_OEM_PERIOD; break; // .
    case '/': windowsKey = VK_OEM_2; break;      // /
    case '[': windowsKey = VK_OEM_4; break;      // [
    case '\\': windowsKey = VK_OEM_5; break;     // \
                                            case ']': windowsKey = VK_OEM_6; break;      // ]
    case '\'': windowsKey = VK_OEM_7; break;     // '
    case '`': windowsKey = VK_OEM_3; break;      // `

        // 其他字符保持不变
    default:
        break;
    }

    if (originalKey != windowsKey || needsShift) {
        Logger::getInstance()->info("Key mapping: 0x" + std::to_string(originalKey) +
            " -> 0x" + std::to_string(windowsKey) +
            (needsShift ? " (with Shift)" : ""));
    }
}

WebRTCManager::~WebRTCManager() {
    Logger::getInstance()->info("WebRTCManager::Destructor - Starting cleanup");
    Cleanup();
    Logger::getInstance()->info("WebRTCManager destructor completed");
}

void WebRTCManager::Cleanup() {
    Logger::getInstance()->info("WebRTCManager::Cleanup - Starting comprehensive cleanup");

    Logger::getInstance()->info("Setting socket runs flag to false");
    socketRuns = false;

    // Release WebRTC resources first
    Logger::getInstance()->info("Releasing WebRTC resources");
    releaseSource();

    Logger::getInstance()->info("Closing writer channel");
    if (writerChannel.is_open()) {
        writerChannel.close();
        Logger::getInstance()->info("Writer channel closed");
    }

    Logger::getInstance()->info("Closing TCP socket");
    if (tcpSocket && tcpSocket->is_open()) {
        boost::system::error_code ec;
        tcpSocket->close(ec);
        if (ec) {
            Logger::getInstance()->warning("Error closing TCP socket: " + ec.message());
        }
        else {
            Logger::getInstance()->info("TCP socket closed successfully");
        }
    }

    Logger::getInstance()->info("Stopping IO context work guards");
    if (ioContextWorkPtr) {
        ioContextWorkPtr.reset();
        Logger::getInstance()->info("IO context work guard reset");
    }

    if (socketIoContextWorkPtr) {
        socketIoContextWorkPtr.reset();
        Logger::getInstance()->info("Socket IO context work guard reset");
    }

    Logger::getInstance()->info("Joining worker threads");
    if (ioContextThread.joinable()) {
        Logger::getInstance()->info("Joining IO context thread");
        ioContextThread.join();
        Logger::getInstance()->info("IO context thread joined");
    }

    if (socketIoContextThread.joinable()) {
        Logger::getInstance()->info("Joining socket IO context thread");
        socketIoContextThread.join();
        Logger::getInstance()->info("Socket IO context thread joined");
    }

    Logger::getInstance()->info("Stopping WebRTC threads");
    if (networkThread) {
        Logger::getInstance()->info("Stopping network thread");
        networkThread->Stop();
        Logger::getInstance()->info("Network thread stopped");
    }

    if (workerThread) {
        Logger::getInstance()->info("Stopping worker thread");
        workerThread->Stop();
        Logger::getInstance()->info("Worker thread stopped");
    }

    if (signalingThread) {
        Logger::getInstance()->info("Stopping signaling thread");
        signalingThread->Stop();
        Logger::getInstance()->info("Signaling thread stopped");
    }

    Logger::getInstance()->info("Cleaning up WebRTC SSL");
    webrtc::CleanupSSL();

    Logger::getInstance()->info("WebRTCManager cleanup completed successfully");
}