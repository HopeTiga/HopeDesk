#include "webrtcremoteclient.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include "Logger.h"

void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnSignalingChange - State changed to: " + std::to_string(static_cast<int>(newState)));

    switch (newState) {
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
        Logger::getInstance()->warning("Unknown signaling state: " + std::to_string(static_cast<int>(newState)));
        break;
    }
}

void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnDataChannel - Data channel received: " + dataChannel->label());
    Logger::getInstance()->info("Data channel state: " + std::to_string(static_cast<int>(dataChannel->state())));

    client_->dataChannel = dataChannel;

    client_->dataChannelObserver = std::make_unique<DataChannelObserverImpl>(client_);
    dataChannel->RegisterObserver(client_->dataChannelObserver.get());
}

void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnIceGatheringChange - ICE gathering state: " + std::to_string(static_cast<int>(newState)));

    switch (newState) {
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
        Logger::getInstance()->warning("Unknown ICE gathering state: " + std::to_string(static_cast<int>(newState)));
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
    client_->sendSignalingMessage(msg);
    Logger::getInstance()->info("ICE candidate sent successfully");
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnIceConnectionChange - ICE connection state: " + std::to_string(static_cast<int>(newState)));

    switch (newState) {
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
        Logger::getInstance()->warning("Unknown ICE connection state: " + std::to_string(static_cast<int>(newState)));
        break;
    }
}

void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) {
    Logger::getInstance()->info("PeerConnectionObserverImpl::OnConnectionChange - Peer connection state: " + std::to_string(static_cast<int>(newState)));

    switch (newState) {
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
        Logger::getInstance()->warning("Unknown peer connection state: " + std::to_string(static_cast<int>(newState)));
        break;
    }
}

void PeerConnectionObserverImpl::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {

    Logger::getInstance()->info("PeerConnectionObserverImpl::OnTrack - Received media track");

    auto receiver = transceiver->receiver();

    auto track = receiver->track();

    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {

        Logger::getInstance()->info("Video track received");

        client_->isReceive = true;

        client_->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
            static_cast<webrtc::VideoTrackInterface*>(track.release())
            );

        client_->videoSink = std::make_unique<VideoTrackSink>(client_);

        client_->videoTrack->AddOrUpdateSink(client_->videoSink.get(), webrtc::VideoSinkWants());

        client_->isReceive = true;
    }
}

void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer) {
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
}

void DataChannelObserverImpl::OnStateChange() {
    if (!client_->dataChannel) {
        return;
    }

    auto state = client_->dataChannel->state();
    Logger::getInstance()->info("DataChannelObserverImpl::OnStateChange - State: " +
                                std::to_string(static_cast<int>(state)));

    switch (state) {
    case webrtc::DataChannelInterface::kOpen:
        Logger::getInstance()->info("Data channel opened");
        break;
    case webrtc::DataChannelInterface::kClosed:
        Logger::getInstance()->info("Data channel closed");
        break;
    case webrtc::DataChannelInterface::kClosing:
        Logger::getInstance()->info("Data channel closing");
        break;
    case webrtc::DataChannelInterface::kConnecting:
        Logger::getInstance()->info("Data channel connecting");
        break;
    }
}

// SetSessionDescriptionObserver implementations
void SetLocalDescriptionObserver::OnSuccess() {
    Logger::getInstance()->info("SetLocalDescriptionObserver::OnSuccess - SetLocalDescription succeeded");
}

void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetLocalDescriptionObserver::OnFailure - SetLocalDescription failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

void SetRemoteDescriptionObserver::OnSuccess() {
    Logger::getInstance()->info("SetRemoteDescriptionObserver::OnSuccess - SetRemoteDescription succeeded");
}

void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetRemoteDescriptionObserver::OnFailure - SetRemoteDescription failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

// CreateSessionDescriptionObserver implementations
void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    Logger::getInstance()->info("CreateOfferObserverImpl::OnSuccess - CreateOffer succeeded");

    if (!desc) {
        Logger::getInstance()->error("CreateOffer success callback received null description");
        return;
    }

    Logger::getInstance()->info("Setting local description from created offer");
    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert offer description to string");
        return;
    }

    Logger::getInstance()->info("Offer SDP length: " + std::to_string(sdp.length()));
    Logger::getInstance()->info("Offer SDP preview: " + sdp.substr(0, 200) + "...");

    boost::json::object msg;
    msg["type"] = "offer";
    msg["sdp"] = sdp;

    Logger::getInstance()->info("Sending offer via signaling message");
    client_->sendSignalingMessage(msg);
    Logger::getInstance()->info("Offer sent successfully");
}

void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateOfferObserverImpl::OnFailure - CreateOffer failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

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
    client_->sendSignalingMessage(msg);
    Logger::getInstance()->info("Answer sent successfully");
}

void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateAnswerObserverImpl::OnFailure - CreateAnswer failed: " + std::string(error.message()));
    Logger::getInstance()->error("Error type: " + std::to_string(static_cast<int>(error.type())));
}

WebRTCRemoteClient::WebRTCRemoteClient( WebRTCRemoteState state)
    : state(state),  webSocket(nullptr), connetState(WebRTCConnetState::none)
    ,channel(ioContext),tcpSocket(nullptr),resolver(nullptr)
    ,ioContextWorkPtr(nullptr),writerChannel(ioContext)
{
    LOG_DEBUG("WebRTCRemoteClient");

    webSocket = std::make_unique<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>>(ioContext);

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this](){

        this->ioContext.run();

    }));

}

void WebRTCRemoteClient::connect(std::string ip)
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
        return;
    }

    // 连接到服务器

    boost::system::error_code ec;

    boost::asio::connect(webSocket->next_layer(), results,ec);

    if(ec){

        Logger::getInstance()->info("connect webSocketServer faild :" + ec.what());

        return;

    }

    // 执行 WebSocket 握手
    webSocket->handshake(host, "/",ec);

    if(ec){

        Logger::getInstance()->info("handshake webSocketServer faild :" + ec.what());

        return;

    }


    webSocketRuns = true;

    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {

        boost::beast::flat_buffer buffer;

        try{
            while (webSocketRuns) {

                co_await  webSocket->async_read(buffer,boost::asio::use_awaitable);

                std::string dataStr = boost::beast::buffers_to_string(buffer.data());

                buffer.consume(buffer.size());

                boost::json::object json = boost::json::parse(dataStr).as_object();

                if(this->state == WebRTCRemoteState::followerRemote && WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::REQUEST){

                    std::shared_ptr<WriterData> writerData =  std::make_shared<WriterData>(dataStr.data(),dataStr.size());

                    writerAsync(writerData);

                    continue;
                }


                if(json.contains("requestType")){
                    int64_t requestType = json["requestType"].as_int64();
                    int64_t responseState = json["state"].as_int64();

                    if(WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER){
                        if(responseState == 200){
                            connetState = WebRTCConnetState::connect;
                            LOG_INFO("%s",json["message"].as_string().c_str());

                            if(!initializePeerConnection()){
                                LOG_ERROR("initializePeerConnection failed!");
                            }
                        }
                    }else if(WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST){

                        if(responseState == 200){
                            LOG_DEBUG("json:%s",boost::json::serialize(json).c_str());

                            if(json.contains("webRTCRemoteState")){
                                WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());

                                // 只在状态未设置时进行角色分配，避免重复处理
                                if(state.load() == WebRTCRemoteState::nullRemote){

                                    // 对方是masterRemote(接收端)，我们需要成为followerRemote(发送端)
                                    if(remoteState == WebRTCRemoteState::masterRemote){

                                        state = WebRTCRemoteState::followerRemote;

                                        targetID = std::string(json["accountID"].as_string().c_str());

                                        this->followData = dataStr;

                                        WindowsServiceManager::deleteService(systemService);

                                        WindowsServiceManager::registerService(systemService, systemServiceExe);

                                        if (WindowsServiceManager::startService(systemService)) {
                                            // 启动写协程
                                            Logger::getInstance()->info("Starting writer coroutine...");

                                            boost::asio::co_spawn(ioContext,[this,dataStr]()mutable->boost::asio::awaitable<void>{

                                                try {
                                                    tcpSocket = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);

                                                    Logger::getInstance()->info("tcpSocket connect start!");

                                                    // 诊断：尝试解析地址
                                                    boost::system::error_code ec;
                                                    auto address = boost::asio::ip::make_address("127.0.0.1", ec);
                                                    if (ec) {
                                                        Logger::getInstance()->error("Failed to parse address 127.0.0.1: " + ec.message());
                                                        co_return;
                                                    }

                                                    boost::asio::ip::tcp::endpoint endpoint(address, 19998);

                                                    // 使用带超时的连接（可选）
                                                    auto connect_result = co_await boost::asio::async_connect(
                                                        *tcpSocket,
                                                        std::array{endpoint},
                                                        boost::asio::use_awaitable
                                                        );

                                                    this->followRemoteHandle();

                                                    socketRuns = true;

                                                    followRunning = true;

                                                    wrtierCoroutineAsync();

                                                    receiveCoroutineAysnc();

                                                    // 发送初始数据
                                                    std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(), dataStr.size());

                                                    Logger::getInstance()->info(dataStr);

                                                    writerAsync(writerData);


                                                } catch (const boost::system::system_error& e) {
                                                    // 只处理这三种连接断开错误
                                                    if (e.code() == boost::asio::error::connection_reset ||
                                                        e.code() == boost::asio::error::connection_aborted ||
                                                        e.code() == boost::asio::error::broken_pipe) {
                                                        Logger::getInstance()->warning("Connection lost in write: " + e.code().message());

                                                    } else {
                                                        // 其他boost错误都当标准异常处理
                                                        Logger::getInstance()->error("Boost system error in write: " + std::string(e.what()));
                                                    }
                                                } catch (const std::exception& e) {
                                                    Logger::getInstance()->error("Exception in write operation: " + std::string(e.what()));
                                                }


                                            },[this](std::exception_ptr p) {
                                                                      LOG_ERROR("writerCoroutineAsync Error");
                                                                  });

                                            continue;
                                        }


                                    }
                                    // 对方是followerRemote(发送端)，我们需要成为masterRemote(接收端)
                                    else if(remoteState == WebRTCRemoteState::followerRemote){
                                        state = WebRTCRemoteState::masterRemote;
                                        targetID = std::string(json["accountID"].as_string().c_str());

                                        // 接收端初始化解码器
                                        // if(!initializeDecoder()){
                                        //     LOG_ERROR("Failed to initialize decoder");
                                        // }
                                    }
                                }
                            }

                            if(json.contains("type")){
                                std::string type(json["type"].as_string().c_str());

                                if(type == "offer"){
                                    // 只有当我们不是发起者时才响应offer
                                    if(!isInit.load()){
                                        Logger::getInstance()->info("Received offer, processing as answerer");

                                        if(json.contains("accountID")){
                                            targetID = std::string(json["accountID"].as_string().c_str());
                                            Logger::getInstance()->info("Set target ID from offer: " + targetID);
                                        }

                                        std::string sdp(json["sdp"].as_string().c_str());
                                        Logger::getInstance()->info("Offer SDP length: " + std::to_string(sdp.length()));
                                        Logger::getInstance()->info("Offer SDP preview: " + sdp.substr(0, 200) + "...");

                                        if(!peerConnection) {
                                            Logger::getInstance()->error("PeerConnection is null, cannot process offer");
                                            continue;
                                        }

                                        // 创建远程描述
                                        webrtc::SdpParseError error;
                                        std::unique_ptr<webrtc::SessionDescriptionInterface> remoteDesc(
                                            webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error));

                                        if(!remoteDesc) {
                                            Logger::getInstance()->error("Failed to parse offer SDP: " + error.description);
                                            continue;
                                        }

                                        Logger::getInstance()->info("Setting remote description (offer)");
                                        peerConnection->SetRemoteDescription(
                                            SetRemoteDescriptionObserver::Create().get(),
                                            remoteDesc.release()
                                            );

                                        // 创建并发送answer
                                        Logger::getInstance()->info("Creating answer");
                                        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

                                        createAnswerObserver = CreateAnswerObserverImpl::Create(this, peerConnection);
                                        peerConnection->CreateAnswer(createAnswerObserver.get(), options);

                                        isInit = true;
                                        Logger::getInstance()->info("Offer processed successfully, answer creation initiated");
                                    } else {
                                        Logger::getInstance()->warning("Received offer but already initialized, ignoring");
                                    }

                                } else if(type == "candidate"){
                                    Logger::getInstance()->info("Received ICE candidate");

                                    std::string candidateStr(json["candidate"].as_string().c_str());
                                    std::string mid = json.contains("mid") ? std::string(json["mid"].as_string().c_str()) : "";
                                    int mlineIndex = json.contains("mlineIndex") ? static_cast<int>(json["mlineIndex"].as_int64()) : 0;

                                    Logger::getInstance()->info("ICE candidate SDP: " + candidateStr.substr(0, 100) + "...");
                                    Logger::getInstance()->info("ICE candidate mid: " + mid);
                                    Logger::getInstance()->info("ICE candidate mline index: " + std::to_string(mlineIndex));

                                    if (peerConnection) {
                                        webrtc::SdpParseError error;
                                        std::unique_ptr<webrtc::IceCandidateInterface> candidate(
                                            webrtc::CreateIceCandidate(mid, mlineIndex, candidateStr, &error));

                                        if(!candidate) {
                                            Logger::getInstance()->error("Failed to parse ICE candidate: " + error.description);
                                            continue;
                                        }

                                        Logger::getInstance()->info("Adding ICE candidate to peer connection");
                                        bool success = peerConnection->AddIceCandidate(candidate.get());

                                        if(success) {
                                            Logger::getInstance()->info("ICE candidate added successfully");
                                        } else {
                                            Logger::getInstance()->error("Failed to add ICE candidate");
                                        }
                                    } else {
                                        Logger::getInstance()->error("PeerConnection is null, cannot add ICE candidate");
                                    }

                                } else {
                                    Logger::getInstance()->warning("Unknown signaling message type: " + type);
                                }
                            }
                        }
                        // 在处理重启消息的地方添加日志
                    }else if(WebRTCRequestState(requestType) == WebRTCRequestState::RESTART){

                        if(responseState == 200){

                            releaseSource();

                            initializePeerConnection();

                            this->state = WebRTCRemoteState::nullRemote;

                            sendRequestToTarget();
                        }
                    }else if(WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE){

                        if(responseState == 200){

                            followRunning = false;

                            releaseSource();

                            initializePeerConnection();

                            this->state = WebRTCRemoteState::nullRemote;

                            this->disConnectRemoteHandle();

                        }
                    }
                }
            }

        }catch(const std::exception& e){

            Logger::getInstance()->error("webSocket Read coroutine Error: " + std::string(e.what()));
        }

    }, [this](std::exception_ptr p) {
                              try {
                                  if (p) {
                                      std::rethrow_exception(p);
                                  }
                              }  catch (const std::exception& e) {
                                  Logger::getInstance()->error("webSocket Read coroutine Error: " + std::string(e.what()));
                              }
                          });

    boost::json::object request;

    request["requestType"] = static_cast<int>(WebRTCRequestState::REGISTER);

    request["accountID"] = this->accountID;

    std::string requestStr = boost::json::serialize(request);

    webSocket->write(boost::asio::buffer(requestStr));
}

WebRTCRemoteClient::~WebRTCRemoteClient()
{

    WindowsServiceManager::stopService(systemService);

    releaseSource();
}

bool WebRTCRemoteClient::initializePeerConnection()
{
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

    return true;
}

void WebRTCRemoteClient::convertYUV420ToRGB24(const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
                                              int width, int height, int yStride, int uStride, int vStride,
                                              uint8_t* rgbData) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int yIndex = y * yStride + x;
            int uvIndex = (y / 2) * uStride + (x / 2);

            int Y = yData[yIndex];
            int U = uData[uvIndex] - 128;
            int V = vData[uvIndex] - 128;

            int R = Y + (1.402f * V);
            int G = Y - (0.344f * U) - (0.714f * V);
            int B = Y + (1.772f * U);

            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));

            int rgbIndex = (y * width + x) * 3;
            if (rgbIndex + 2 < width * height * 3) {
                rgbData[rgbIndex] = static_cast<uint8_t>(R);
                rgbData[rgbIndex + 1] = static_cast<uint8_t>(G);
                rgbData[rgbIndex + 2] = static_cast<uint8_t>(B);
            }
        }
    }
}

void WebRTCRemoteClient:: writerAsync(std::shared_ptr<WriterData> writerData){

    writerDataQueues.enqueue(writerData);

    if (writerChannel.is_open()) {

        writerChannel.try_send(boost::system::error_code{});

    }
}

void WebRTCRemoteClient::disConnectRemote()
{
    followRunning = false;

    releaseSource();

    initializePeerConnection();

    this->state = WebRTCRemoteState::nullRemote;

    if(webSocket && webSocket->is_open()){
        boost::json::object message;
        message["accountID"] = this->accountID;
        message["targetID"] = this->targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);
        std::string messageStr = boost::json::serialize(message);
        webSocket->write(boost::asio::buffer(messageStr));
    }
}


void WebRTCRemoteClient::wrtierCoroutineAsync()
{

    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        Logger::getInstance()->info("Writer coroutine started");

        try {
            for (;;) {
                // 注意：这里的逻辑可能有问题
                // 原逻辑：队列为空 AND socketRuns为false 时等待
                // 正确逻辑应该是：队列为空时等待，除非socketRuns为false
                while (this->writerDataQueues.size_approx() == 0 && this->socketRuns.load()) {
                    Logger::getInstance()->info("Writer waiting for data... Queue size: " +
                                                std::to_string(this->writerDataQueues.size_approx()) +
                                                ", socketRuns: " + std::to_string(this->socketRuns.load()));

                    try {
                        co_await this->writerChannel.async_receive(boost::asio::use_awaitable);
                        Logger::getInstance()->info("Writer channel received signal");
                    } catch (const boost::system::system_error& e) {
                        Logger::getInstance()->error("Writer channel receive error: " + std::string(e.what()));
                        Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());

                        // 如果channel被关闭，退出循环
                        if (e.code() == boost::asio::error::operation_aborted) {
                            Logger::getInstance()->info("Writer channel closed, exiting writer coroutine");
                            co_return;
                        }
                    }
                }

                // 检查是否需要退出
                if (!socketRuns) {
                    Logger::getInstance()->info("socketRuns is false, processing remaining queue before exit");

                    // 处理剩余的队列数据
                    while (this->writerDataQueues.size_approx() > 0) {
                        std::shared_ptr<WriterData> nowNode = nullptr;

                        if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                            try {
                                Logger::getInstance()->info("Sending remaining data, size: " +
                                                            std::to_string(nowNode->size));

                                co_await boost::asio::async_write(*this->tcpSocket,
                                                                  boost::asio::buffer(nowNode->data, nowNode->size),
                                                                  boost::asio::use_awaitable);

                            }
                            catch (const boost::system::system_error& e) {
                                Logger::getInstance()->error("Error sending message during shutdown: " + std::string(e.what()));
                                Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                                break; // 发送失败，退出循环
                            }
                            catch (const std::exception& e) {
                                Logger::getInstance()->error("Unexpected error during shutdown send: " + std::string(e.what()));
                                break;
                            }
                        }
                    }

                    co_return; // 退出协程
                }

                // 正常处理队列中的数据
                std::shared_ptr<WriterData> nowNode = nullptr;
                if (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                    try {
                        Logger::getInstance()->info("Sending data, size: " + std::to_string(nowNode->size));

                        // 检查socket状态
                        if (!tcpSocket || !tcpSocket->is_open()) {
                            Logger::getInstance()->error("Socket is not open, cannot send data");
                            continue;
                        }

                        co_await boost::asio::async_write(*this->tcpSocket,
                                                          boost::asio::buffer(nowNode->data, nowNode->size),
                                                          boost::asio::use_awaitable);

                        Logger::getInstance()->info("Data sent successfully");

                    } catch (const boost::system::system_error& e) {
                        Logger::getInstance()->error("Error sending data: " + std::string(e.what()));
                        Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());

                        // 检查特定错误
                        if (e.code() == boost::asio::error::broken_pipe ||
                            e.code() == boost::asio::error::connection_reset ||
                            e.code() == boost::asio::error::connection_aborted) {
                            Logger::getInstance()->error("Connection lost, exiting writer coroutine");

                            co_return;
                        }
                    } catch (const std::exception& e) {
                        Logger::getInstance()->error("Unexpected error sending data: " + std::string(e.what()));
                    }
                } else {
                    Logger::getInstance()->debug("No data to send or dequeue failed");
                }
            }
        } catch (const std::exception& e) {
            Logger::getInstance()->error("Writer coroutine unhandled exception: " + std::string(e.what()));
        } catch (...) {
            Logger::getInstance()->error("Writer coroutine unknown exception");
        }

        Logger::getInstance()->info("Writer coroutine ended");
        co_return;

    }, [this](std::exception_ptr p) {
                              try {
                                  if (p) {
                                      std::rethrow_exception(p);
                                  }
                              } catch (const boost::system::system_error& e) {
                                  Logger::getInstance()->error("Writer coroutine system error: " + std::string(e.what()));
                                  Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                                  Logger::getInstance()->error("Error category: " + std::string(e.code().category().name()));
                              } catch (const std::exception& e) {
                                  Logger::getInstance()->error("Writer coroutine standard exception: " + std::string(e.what()));
                              } catch (...) {
                                  Logger::getInstance()->error("Writer coroutine unknown exception");
                              }
                          });
}

void WebRTCRemoteClient::receiveCoroutineAysnc()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        Logger::getInstance()->info("Reader coroutine started");

        try {
            char headerBuffer[8];
            size_t headerSize = sizeof(int64_t);
            int messageCount = 0;

            while (socketRuns) {
                std::memset(headerBuffer, 0, headerSize);

                Logger::getInstance()->debug("Waiting to receive message header #" + std::to_string(++messageCount));

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
                        Logger::getInstance()->debug("Read " + std::to_string(n) + " bytes of header, total: " +
                                                     std::to_string(headerRead) + "/" + std::to_string(headerSize));
                    }
                    catch (const boost::system::system_error& e) {
                        Logger::getInstance()->error("Error reading header: " + std::string(e.what()));
                        Logger::getInstance()->error("Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
                        if(e.code().value() == 10054){
                            handleAsioException();
                        }
                        co_return;
                    }
                }

                int64_t rawBodyLength = 0;
                std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
                int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

                // ✅ 修复：正确的字符串拼接
                Logger::getInstance()->info("Message bodySize: " + std::to_string(bodyLength));

                if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) { // 限制最大10MB
                    Logger::getInstance()->error("Invalid body length: " + std::to_string(bodyLength));
                    co_return;
                }

                size_t bodySize = static_cast<size_t>(bodyLength);

                // ✅ 使用智能指针管理内存
                std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]); // +1 for null terminator
                if (!bodyBuffer) {
                    Logger::getInstance()->error("Failed to allocate memory for body buffer");
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
                            Logger::getInstance()->info("Connection closed by peer (body read returned 0)");
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

                std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(bodyBuffer.get(),bodyLength);

                std::string bodyStr(bodyBuffer.get(), bodySize);

                if(webSocket && webSocket->is_open()){

                    webSocket->write(boost::asio::buffer(bodyStr));

                }
            }
        }
        catch (const std::exception& e) {
            Logger::getInstance()->error("Reader coroutine exception: " + std::string(e.what()));
        }
        catch (...) {
            Logger::getInstance()->error("Reader coroutine unknown exception");
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
                                  Logger::getInstance()->error("Reader coroutine handler exception: " + std::string(e.what()));
                              }
                          });
}

void WebRTCRemoteClient::sendSignalingMessage(boost::json::object& msg) {
    if (!webSocket || !webSocket->is_open()) {
        Logger::getInstance()->error("Cannot send signaling message - WebSocket not connected");
        return;
    }

    // 添加发送者信息
    msg["accountID"] = accountID;
    if (!targetID.empty()) {
        msg["targetID"] = targetID;
    }

    msg["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);

    msg["state"] = 200;

    std::string messageStr = boost::json::serialize(msg);
    Logger::getInstance()->debug("Sending signaling message: " + messageStr.substr(0, 200));

    try {
        webSocket->write(boost::asio::buffer(messageStr));
    } catch (const std::exception& e) {
        Logger::getInstance()->error("Failed to send signaling message: " + std::string(e.what()));
    }
}

void WebRTCRemoteClient::handleAsioException()
{

    if(!followRunning) return;

    this->state = WebRTCRemoteState::nullRemote;

    if(webSocket){

        boost::json::object message;

        message["accountID"] = this->accountID;

        message["targetID"] = this->targetID;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::RESTART);

        std::string messageStr = boost::json::serialize(message);

        webSocket->write(boost::asio::buffer(messageStr));

        WindowsServiceManager::stopService(systemService);
    }

}

void WebRTCRemoteClient::releaseSource()
{
    // 1. 移除VideoSink
    if (videoTrack && videoSink) {
        auto video_track = static_cast<webrtc::VideoTrackInterface*>(videoTrack.get());
        video_track->RemoveSink(videoSink.get());
        videoSink.reset();
    }

    // 2. 关闭连接
    if (peerConnection) {

        peerConnection->Close();
    }


    if (dataChannel) {

        dataChannel->Close();
    }

    // 3. 清理Observer
    peerConnectionObserver.reset();
    dataChannelObserver.reset();

    // 4. 释放WebRTC对象引用
    createOfferObserver = nullptr;
    createAnswerObserver = nullptr;
    videoSender = nullptr;
    dataChannel = nullptr;
    videoTrack = nullptr;
    peerConnection = nullptr;

    // 5. 释放PeerConnectionFactory
    peerConnectionFactory = nullptr;

    // 6. 停止并释放线程
    if (signalingThread) {

        signalingThread->Stop();
        signalingThread.reset();
    }
    if (workerThread) {

        workerThread->Stop();
        workerThread.reset();
    }
    if (networkThread) {

        networkThread->Stop();
        networkThread.reset();
    }

    isInit = false;
    isReceive = false;

    // 关闭channel，让等待的协程退出
    if(channel.is_open()){

        channel.try_send(boost::system::error_code{});

    }

    if(writerChannel.is_open()){

        writerChannel.try_send(boost::system::error_code{});

    }

    // 清空所有队列
    while (!trackFrameQueues.empty()) {
        trackFrameQueues.pop();
    }

    while (!remoteBinaryQueue.empty()) {
        remoteBinaryQueue.pop();
    }


    if(state == WebRTCRemoteState::followerRemote){

        WindowsServiceManager::stopService(systemService);

    }

}


std::string WebRTCRemoteClient::getAccountID() const
{
    return accountID;
}

void WebRTCRemoteClient::setAccountID(const std::string &newAccountID)
{
    accountID = newAccountID;
}

void WebRTCRemoteClient::sendRequestToTarget()
{
    if (targetID.empty()) {
        LOG_ERROR("Target ID not set");
        return;
    }

    if (!webSocket || !webSocket->is_open()) {
        LOG_ERROR("WebSocket not connected");
        return;
    }

    if(peerConnection != nullptr){
        // 避免重复设置状态
        if (state.load() != WebRTCRemoteState::nullRemote) {
            LOG_WARNING("State already set, current state: %d", static_cast<int>(state.load()));
            return;
        }

        // 主动发起连接的一方成为接收端
        state = WebRTCRemoteState::masterRemote;

        boost::json::object message;
        message["accountID"] = accountID;
        message["targetID"] = targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
        message["webRTCRemoteState"] = static_cast<int64_t>(state.load());

        webSocket->write(boost::asio::buffer(boost::json::serialize(message)));
    }

    LOG_INFO("Request sent to target: %s", targetID.c_str());
}

std::string WebRTCRemoteClient::getTargetID() const
{
    return targetID;
}

void WebRTCRemoteClient::setTargetID(const std::string &newTargetID)
{
    targetID = newTargetID;
}

void WebRTCRemoteClient::writerRemote(char *data, size_t size)
{
    if(state.load() == WebRTCRemoteState::masterRemote){
        if(!dataChannel) {
            LOG_ERROR("DataChannel is null");
            delete[] data;
            return;
        }

        try {

            webrtc::CopyOnWriteBuffer buffer(data, size);

            webrtc::DataBuffer dataBuffer(buffer, true); // true 表示二进制数据

            // 发送数据
            bool success = dataChannel->Send(dataBuffer);

            delete [] data;

            if(!success) {
                LOG_ERROR("Failed to send data");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception while sending data: %s", e.what());
        }
    }
}

void WebRTCRemoteClient::setVideoFrameCallback(VideoFrameCallback callback)
{
    this->videoFrameCallback = callback;
}

void WebRTCRemoteClient::disConnect()
{
    webSocketRuns = false;

    if (webSocket && webSocket->is_open()) {

        try{

            webSocket->next_layer().cancel();

            webSocket->close(boost::beast::websocket::close_code::normal);

        }catch(std::exception &e){

            Logger::getInstance()->info(e.what());

        }
    }


    if(state == WebRTCRemoteState::followerRemote){

        socketRuns = false;

        followRunning = false;

        if(tcpSocket && tcpSocket->is_open()){

            tcpSocket->close();

        }

        WindowsServiceManager::stopService(systemService);


    }

    releaseSource();

    connetState = WebRTCConnetState::none;

}


void VideoTrackSink::OnFrame(const webrtc::VideoFrame &frame)
{
    if (!client_) return;

    // 获取帧数据
    int width = frame.width();
    int height = frame.height();

    // 转换为I420格式
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer =
        frame.video_frame_buffer()->ToI420();

    // 创建RGB缓冲区
    auto rgbFrame = std::make_shared<VideoFrame>(width, height);

    // YUV转RGB
    client_->convertYUV420ToRGB24(
        i420_buffer->DataY(),
        i420_buffer->DataU(),
        i420_buffer->DataV(),
        width, height,
        i420_buffer->StrideY(),
        i420_buffer->StrideU(),
        i420_buffer->StrideV(),
        rgbFrame->data.get()
        );

    // 触发回调
    if (client_->videoFrameCallback) {
        client_->videoFrameCallback(rgbFrame);
    }
}
