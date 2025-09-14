#include "webrtcremoteclient.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include "Logger.h"

void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) {
    switch (newState) {
    case webrtc::PeerConnectionInterface::kStable:
        Logger::getInstance()->info("Signaling state: kStable");
        break;
    case webrtc::PeerConnectionInterface::kHaveLocalOffer:
        break;
    case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
        break;
    case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
        break;
    case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
        break;
    case webrtc::PeerConnectionInterface::kClosed:
        Logger::getInstance()->info("Signaling state: kClosed");
        break;
    default:
        break;
    }
}

void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) {
    Logger::getInstance()->info("Data channel received: " + dataChannel->label());

    client->dataChannel = dataChannel;
    client->dataChannelObserver = std::make_unique<DataChannelObserverImpl>(client);
    dataChannel->RegisterObserver(client->dataChannelObserver.get());
}

void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) {
    switch (newState) {
    case webrtc::PeerConnectionInterface::kIceGatheringComplete:
        Logger::getInstance()->info("ICE gathering complete");
        break;
    default:
        break;
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

    client->sendSignalingMessage(msg);
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) {
    switch (newState) {
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
        Logger::getInstance()->info("WebRTC connection established");

        break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
        Logger::getInstance()->error("ICE connection failed");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
        Logger::getInstance()->warning("ICE connection disconnected");
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
        Logger::getInstance()->info("ICE connection closed");
        break;
    default:
        break;
    }
}

void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) {
    switch (newState) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:{

        Logger::getInstance()->info("Peer connection established");

        client->isRemote = true;

        client->state = WebRTCRemoteState::masterRemote;

        if(client->remoteSuccessFulHandle){

            client->remoteSuccessFulHandle();

        }

        break;
    }
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:{

        Logger::getInstance()->warning("Peer connection disconnected");

        client->disConnectHandle();

        break;
    }
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:

        Logger::getInstance()->error("Peer connection failed");

        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:

        Logger::getInstance()->info("Peer connection closed");
        break;
    default:
        break;
    }
}

void PeerConnectionObserverImpl::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    auto receiver = transceiver->receiver();
    auto track = receiver->track();

    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        Logger::getInstance()->info("Video track received");

        client->isReceive = true;
        client->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
            static_cast<webrtc::VideoTrackInterface*>(track.release())
            );
        client->videoSink = std::make_unique<VideoTrackSink>(client);
        client->videoTrack->AddOrUpdateSink(client->videoSink.get(), webrtc::VideoSinkWants());
        client->isReceive = true;
    }
}

void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer) {
    if (buffer.size() == 0) {
        Logger::getInstance()->warning("Received empty data channel message");
        return;
    }

    if (buffer.size() > 1024 * 1024) { // 1MB limit
        Logger::getInstance()->error("Received oversized data channel message: " + std::to_string(buffer.size()) + " bytes");
        return;
    }

    const unsigned char * data = reinterpret_cast<const unsigned char*>(buffer.data.data());
    size_t size = buffer.size();

    short type = -1;
    memcpy(&type, data, sizeof(short));

    switch(type) {
    case 0: { // 光标索引消息

        int index, width, height, hotX, hotY;
        memcpy(&index, data + sizeof(short), sizeof(int));
        memcpy(&width, data + sizeof(short) + sizeof(int), sizeof(int));
        memcpy(&height, data + sizeof(short) + 2 * sizeof(int), sizeof(int));
        memcpy(&hotX, data + sizeof(short) + 3 * sizeof(int), sizeof(int));
        memcpy(&hotY, data + sizeof(short) + 4 * sizeof(int), sizeof(int));

        // 获取光标数据
        std::vector<unsigned char>& cursorData = client->cursorArray[index];
        // 创建光标
        HCURSOR cursor = CreateCursorFromRGBA(cursorData.data(), width, height, hotX, hotY);
        if (cursor) {
            SetSystemCursor(CopyCursor(cursor), 32512);
        }
        break;
    }
    case 1: { // 新光标数据

        int index, width, height, hotX, hotY;
        memcpy(&index, data + sizeof(short), sizeof(int));
        memcpy(&width, data + sizeof(short) + sizeof(int), sizeof(int));
        memcpy(&height, data + sizeof(short) + 2 * sizeof(int), sizeof(int));
        memcpy(&hotX, data + sizeof(short) + 3 * sizeof(int), sizeof(int));
        memcpy(&hotY, data + sizeof(short) + 4 * sizeof(int), sizeof(int));

        // 计算图像数据部分的大小
        size_t headerSize = sizeof(short) + 5 * sizeof(int);
        size_t imageSize = size - headerSize;

        // 将图像数据部分存储到cursorArray中
        std::vector<unsigned char> cursorData(imageSize);
        memcpy(cursorData.data(), data + headerSize, imageSize);

        // 如果索引等于当前cursorArray的大小，则添加新元素
        if (index == client->cursorArray.size()) {
            client->cursorArray.push_back(cursorData);
        } else if (index < client->cursorArray.size()) {
            client->cursorArray[index] = cursorData;
        } else {
            // 索引无效
            Logger::getInstance()->error("Received invalid index for type 1 message: " + std::to_string(index));
            break;
        }

        // 创建光标
        HCURSOR cursor = CreateCursorFromRGBA(cursorData.data(), width, height, hotX, hotY);
        if (cursor) {
            SetSystemCursor(CopyCursor(cursor), 32512);
        }
        break;
    }
    default: {
        Logger::getInstance()->warning("Received unknown data channel message type: " + std::to_string(type));
        break;
    }
    }
}



void DataChannelObserverImpl::OnStateChange() {
    if (!client->dataChannel) {
        return;
    }

    auto state = client->dataChannel->state();
    switch (state) {
    case webrtc::DataChannelInterface::kOpen:
        Logger::getInstance()->info("Data channel opened");
        break;
    case webrtc::DataChannelInterface::kClosed:
        Logger::getInstance()->info("Data channel closed");
        break;
    default:
        break;
    }
}

// SetSessionDescriptionObserver implementations
void SetLocalDescriptionObserver::OnSuccess() {
}

void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetLocalDescription failed: " + std::string(error.message()));
}

void SetRemoteDescriptionObserver::OnSuccess() {
}

void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("SetRemoteDescription failed: " + std::string(error.message()));
}

// CreateSessionDescriptionObserver implementations
void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc) {
        Logger::getInstance()->error("CreateOffer success callback received null description");
        return;
    }

    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert offer description to string");
        return;
    }

    boost::json::object msg;
    msg["type"] = "offer";
    msg["sdp"] = sdp;

    client->sendSignalingMessage(msg);
}

void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateOffer failed: " + std::string(error.message()));
}

void CreateAnswerObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc) {
        Logger::getInstance()->error("CreateAnswer success callback received null description");
        return;
    }

    peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

    std::string sdp;
    if (!desc->ToString(&sdp)) {
        Logger::getInstance()->error("Failed to convert answer description to string");
        return;
    }

    boost::json::object msg;
    msg["type"] = "answer";
    msg["sdp"] = sdp;

    client->sendSignalingMessage(msg);
}

void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
    Logger::getInstance()->error("CreateAnswer failed: " + std::string(error.message()));
}

WebRTCRemoteClient::WebRTCRemoteClient(WebRTCRemoteState state)
    : state(state), webSocket(nullptr), connetState(WebRTCConnetState::none)
    ,channel(ioContext),tcpSocket(nullptr),resolver(nullptr)
    ,ioContextWorkPtr(nullptr),writerChannel(ioContext)
{
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

    if (!webSocket) {
        // 通知连接失败
        if (webSocketConnectedCallback) {
            webSocketConnectedCallback(false);
        }
        return;
    }


    // 连接到服务器
    boost::system::error_code ec;
    boost::asio::connect(webSocket->next_layer(), results,ec);

    if(ec){
        Logger::getInstance()->info("connect webSocketServer failed :" + ec.what());
        if (webSocketConnectedCallback) {
            webSocketConnectedCallback(false);
        }
        return;
    }

    // 执行 WebSocket 握手
    webSocket->handshake(host, "/",ec);

    if(ec){
        Logger::getInstance()->info("handshake webSocketServer failed :" + ec.what());
        if (webSocketConnectedCallback) {
            webSocketConnectedCallback(false);
        }
        return;
    }

    webSocketRuns = true;

    if (webSocketConnectedCallback) {
        webSocketConnectedCallback(true);
    }

    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        boost::beast::flat_buffer buffer;
        try{
            while (webSocketRuns) {
                co_await webSocket->async_read(buffer,boost::asio::use_awaitable);
                std::string dataStr = boost::beast::buffers_to_string(buffer.data());
                buffer.consume(buffer.size());
                boost::json::object json = boost::json::parse(dataStr).as_object();

                if(this->tcpSocket&& this->state == WebRTCRemoteState::followerRemote && WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::REQUEST){
                    std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(),dataStr.size());
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
                            if(json.contains("webRTCRemoteState")){
                                WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());

                                // 只在状态未设置时进行角色分配，避免重复处理
                                if(!isRemote){
                                    // 对方是masterRemote(接收端)，我们需要成为followerRemote(发送端)
                                    if(remoteState == WebRTCRemoteState::masterRemote){

                                        state = WebRTCRemoteState::followerRemote;

                                        targetID = std::string(json["accountID"].as_string().c_str());

                                        this->followData = dataStr;

                                        WindowsServiceManager::stopService(systemService);

                                        if(!WindowsServiceManager::serviceExists(systemService)){

                                            WindowsServiceManager::registerService(systemService, systemServiceExe);

                                        }

                                        if (WindowsServiceManager::startService(systemService)) {

                                            boost::asio::co_spawn(ioContext,[this,dataStr]()mutable->boost::asio::awaitable<void>{
                                                try {
                                                    tcpSocket = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);

                                                    // 诊断：尝试解析地址
                                                    boost::system::error_code ec;
                                                    auto address = boost::asio::ip::make_address("127.0.0.1", ec);
                                                    if (ec) {
                                                        Logger::getInstance()->error("Failed to parse address 127.0.0.1: " + ec.message());
                                                        co_return;
                                                    }

                                                    boost::asio::ip::tcp::endpoint endpoint(address, 19998);

                                                    // 使用带超时的连接（可选）
                                                    auto connectResult = co_await boost::asio::async_connect(
                                                        *tcpSocket,
                                                        std::array{endpoint},
                                                        boost::asio::use_awaitable
                                                        );

                                                    socketRuns = true;

                                                    followRunning = true;

                                                    wrtierCoroutineAsync();

                                                    receiveCoroutineAysnc();

                                                    // 发送初始数据
                                                    std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(), dataStr.size());

                                                    writerAsync(writerData);

                                                } catch (const boost::system::system_error& e) {
                                                    // 只处理这三种连接断开错误
                                                    if (e.code() == boost::asio::error::connection_reset ||
                                                        e.code() == boost::asio::error::connection_aborted ||
                                                        e.code() == boost::asio::error::broken_pipe) {
                                                        handleAsioException();
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
                                    }
                                }
                            }

                            if(json.contains("type")){
                                std::string type(json["type"].as_string().c_str());

                                if(type == "offer"){
                                    // 只有当我们不是发起者时才响应offer
                                    if(!isInit.load()){
                                        if(json.contains("accountID")){
                                            targetID = std::string(json["accountID"].as_string().c_str());
                                        }

                                        std::string sdp(json["sdp"].as_string().c_str());

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

                                        peerConnection->SetRemoteDescription(
                                            SetRemoteDescriptionObserver::Create().get(),
                                            remoteDesc.release()
                                            );

                                        // 创建并发送answer
                                        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                                        createAnswerObserver = CreateAnswerObserverImpl::Create(this, peerConnection);
                                        peerConnection->CreateAnswer(createAnswerObserver.get(), options);
                                        isInit = true;

                                    }

                                } else if(type == "candidate"){
                                    std::string candidateStr(json["candidate"].as_string().c_str());
                                    std::string mid = json.contains("mid") ? std::string(json["mid"].as_string().c_str()) : "";
                                    int mlineIndex = json.contains("mlineIndex") ? static_cast<int>(json["mlineIndex"].as_int64()) : 0;

                                    if (peerConnection) {
                                        webrtc::SdpParseError error;
                                        std::unique_ptr<webrtc::IceCandidateInterface> candidate(
                                            webrtc::CreateIceCandidate(mid, mlineIndex, candidateStr, &error));

                                        if(!candidate) {
                                            Logger::getInstance()->error("Failed to parse ICE candidate: " + error.description);
                                            continue;
                                        }

                                        bool success = peerConnection->AddIceCandidate(candidate.get());
                                        if(!success) {
                                            Logger::getInstance()->error("Failed to add ICE candidate");
                                        }
                                    } else {
                                        Logger::getInstance()->error("PeerConnection is null, cannot add ICE candidate");
                                    }
                                }
                            }
                        }
                        else if(responseState == 404){

                            if(remoteFailedHandle){

                                remoteFailedHandle();

                            }

                        }

                    }else if(WebRTCRequestState(requestType) == WebRTCRequestState::RESTART){

                        if(responseState == 200){

                            if(isRemote == false) continue;

                            releaseSource();

                            initializePeerConnection();

                            this->state = WebRTCRemoteState::nullRemote;

                            sendRequestToTarget();
                        }
                    }else if(WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE){

                        if(responseState == 200){

                            disConnectHandle();

                        }

                    }
                }
            }
        }catch(const std::exception& e){
            Logger::getInstance()->error("webSocket Read coroutine Error: " + std::string(e.what()));

            if(webSocketRuns){

                if(webSocketDisConnect){
                    webSocketDisConnect(e);
                }

            }

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

    peerConnectionFactory.release();

    networkThread.reset();

    workerThread.reset();

    signalingThread.reset();
}

bool WebRTCRemoteClient::initializePeerConnection()
{
    // Clean up any existing connection first
    if (peerConnection) {
        releaseSource();
    }

    webrtc::InitializeSSL();

    if (!peerConnectionFactory) {
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
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

    config.ice_connection_receiving_timeout = 10000;        // 5秒无数据包则认为断开

    config.ice_unwritable_timeout = 10000;                  // 3秒无响应则标记为不可写

    config.ice_inactive_timeout = 10000;                    // 5秒后标记为非活跃

    config.set_dscp(true);

    webrtc::PeerConnectionInterface::IceServer stunServer;
    stunServer.uri = "stun:150.158.173.80:3478";
    config.servers.push_back(stunServer);

    webrtc::PeerConnectionInterface::IceServer turnServer;
    turnServer.uri = "turn:150.158.173.80:3478";
    turnServer.username = "HopeTiga";
    turnServer.password = "dy913140924";
    config.servers.emplace_back(turnServer);

    peerConnectionObserver = std::make_unique<PeerConnectionObserverImpl>(this);
    webrtc::PeerConnectionDependencies pcDependencies(peerConnectionObserver.get());

    auto pcResult = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pcDependencies));
    if (!pcResult.ok()) {
        Logger::getInstance()->error("Failed to create PeerConnection: " + std::string(pcResult.error().message()));
        return false;
    }

    peerConnection = pcResult.MoveValue();
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

void WebRTCRemoteClient::writerAsync(std::shared_ptr<WriterData> writerData){
    writerDataQueues.enqueue(writerData);
    if (writerChannel.is_open()) {
        writerChannel.try_send(boost::system::error_code{});
    }
}

void WebRTCRemoteClient::disConnectRemote()
{

    this->state = WebRTCRemoteState::nullRemote;

    if(isRemote == false) return;

    isRemote = false;

    releaseSource();

    initializePeerConnection();

    WindowsServiceManager::stopService(systemService);

    if(webSocket && webSocket->is_open()){
        boost::json::object message;
        message["accountID"] = this->accountID;
        message["targetID"] = this->targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);
        std::string messageStr = boost::json::serialize(message);
        webSocket->write(boost::asio::buffer(messageStr));
    }
}

void WebRTCRemoteClient::disConnectHandle()
{
    this->state = WebRTCRemoteState::nullRemote;

    if(isRemote == false) return;

    isRemote = false;

    if(disConnectRemoteHandle){

        disConnectRemoteHandle();

    }

    releaseSource();

    WindowsServiceManager::stopService(systemService);

    initializePeerConnection();

}

void WebRTCRemoteClient::wrtierCoroutineAsync()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        try {
            for (;;) {

                std::shared_ptr<WriterData> nowNode = nullptr;
                while (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                    try {
                        // 检查socket状态
                        if (!tcpSocket || !tcpSocket->is_open()) {
                            Logger::getInstance()->error("Socket is not open, cannot send data");
                            continue;
                        }

                        co_await boost::asio::async_write(*this->tcpSocket,
                                                          boost::asio::buffer(nowNode->data, nowNode->size),
                                                          boost::asio::use_awaitable);

                    } catch (const boost::system::system_error& e) {
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
                }

                if (!socketRuns) {
                    // 处理剩余的队列数据
                    std::shared_ptr<WriterData> nowNode = nullptr;

                    while (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                        try {
                            co_await boost::asio::async_write(*this->tcpSocket,
                                                              boost::asio::buffer(nowNode->data, nowNode->size),
                                                              boost::asio::use_awaitable);
                        }
                        catch (const boost::system::system_error& e) {
                            break; // 发送失败，退出循环
                        }
                        catch (const std::exception& e) {
                            break;
                        }

                    }
                    co_return; // 退出协程
                }else{
                    co_await this->writerChannel.async_receive(boost::asio::use_awaitable);
                }

            }
        } catch (const std::exception& e) {
            Logger::getInstance()->error("Writer coroutine unhandled exception: " + std::string(e.what()));
        } catch (...) {
            Logger::getInstance()->error("Writer coroutine unknown exception");
        }
        co_return;
    }, [this](std::exception_ptr p) {
                              try {
                                  if (p) {
                                      std::rethrow_exception(p);
                                  }
                              } catch (const std::exception& e) {
                                  Logger::getInstance()->error("Writer coroutine exception: " + std::string(e.what()));
                              }
                          });
}

void WebRTCRemoteClient::receiveCoroutineAysnc()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        try {
            char headerBuffer[8];
            size_t headerSize = sizeof(int64_t);
            int messageCount = 0;

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

                        handleAsioException();

                        co_return;
                    }
                }

                int64_t rawBodyLength = 0;
                std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
                int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

                if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) { // 限制最大10MB
                    Logger::getInstance()->error("Invalid body length: " + std::to_string(bodyLength));
                    co_return;
                }

                size_t bodySize = static_cast<size_t>(bodyLength);

                // 使用智能指针管理内存
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
                            co_return;
                        }
                        bodyRead += n;
                    }
                    catch (const boost::system::system_error& e) {
                        co_return;
                    }
                }

                std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(bodyBuffer.get(),bodyLength);
                std::string bodyStr(bodyBuffer.get(), bodySize);

                boost::json::object json = boost::json::parse(bodyStr).as_object();

                if(WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::START){

                    this->followRemoteHandle();

                    this->isRemote = true;

                    continue;

                }else if(WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::CLOSE){

                    disConnectHandle();

                    continue;
                }

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

    try {
        webSocket->write(boost::asio::buffer(messageStr));
    } catch (const std::exception& e) {
        Logger::getInstance()->error("Failed to send signaling message: " + std::string(e.what()));
    }
}

void WebRTCRemoteClient::handleAsioException()
{

    this->state = WebRTCRemoteState::nullRemote;

    if(webSocket && webSocket->is_open()){

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

    if (videoTrack && videoSink) {
        auto videoTrack = static_cast<webrtc::VideoTrackInterface*>(this->videoTrack.get());
        videoTrack->RemoveSink(videoSink.get());

        videoSink.reset();
    }

    if (peerConnection) {
        peerConnection->Close();

    }

    if (dataChannel) {

        dataChannel->Close();

    }

    peerConnectionObserver.reset();

    dataChannelObserver.reset();

    createOfferObserver = nullptr;

    createAnswerObserver = nullptr;

    videoSender = nullptr;

    dataChannel = nullptr;

    videoTrack = nullptr;

    peerConnection = nullptr;

    isInit = false;

    isReceive = false;

    if(channel.is_open()){

        channel.try_send(boost::system::error_code{});

    }

    if(writerChannel.is_open()){

        writerChannel.try_send(boost::system::error_code{});
    }

    while (!trackFrameQueues.empty()) {
        trackFrameQueues.pop();
    }


    while (!remoteBinaryQueue.empty()) {
        remoteBinaryQueue.pop();
    }


    if(state == WebRTCRemoteState::followerRemote){
        WindowsServiceManager::stopService(systemService);  // ← 也可能在这里阻塞
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

        boost::json::object message;
        message["accountID"] = accountID;
        message["targetID"] = targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
        message["webRTCRemoteState"] = static_cast<int64_t>(WebRTCRemoteState::masterRemote);

        webSocket->write(boost::asio::buffer(boost::json::serialize(message)));

        LOG_INFO("Request sent to target: %s", targetID.c_str());
    }


}

std::string WebRTCRemoteClient::getTargetID() const
{
    return targetID;
}

void WebRTCRemoteClient::setTargetID(const std::string &newTargetID)
{
    targetID = newTargetID;
}

void WebRTCRemoteClient::writerRemote(unsigned char *data, size_t size)
{
    if(state.load() == WebRTCRemoteState::masterRemote){

        if(!dataChannel) {

            LOG_ERROR("DataChannel is null");

            delete[] data;

            return;
        }

        webrtc::CopyOnWriteBuffer buffer(data, size);

        webrtc::DataBuffer dataBuffer(buffer, true); // true 表示二进制数据

        dataChannel->SendAsync(dataBuffer,[this,data](webrtc::RTCError){

            delete [] data;

        });
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

    this->state == WebRTCRemoteState::nullRemote;

    isRemote = false;

    disConnectHandle();
}

void VideoTrackSink::OnFrame(const webrtc::VideoFrame &frame)
{
    if (!client) return;


    // 获取帧数据
    int width = frame.width();
    int height = frame.height();

    // 转换为I420格式
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer =
        frame.video_frame_buffer()->ToI420();

    // 创建RGB缓冲区
    auto rgbFrame = std::make_shared<VideoFrame>(width, height);

    // YUV转RGB
    client->convertYUV420ToRGB24(
        i420Buffer->DataY(),
        i420Buffer->DataU(),
        i420Buffer->DataV(),
        width, height,
        i420Buffer->StrideY(),
        i420Buffer->StrideU(),
        i420Buffer->StrideV(),
        rgbFrame->data.get()
        );

    // 触发回调
    if (client->videoFrameCallback) {
        client->videoFrameCallback(rgbFrame);
    }
}
