#include "WebRTCManager.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/field_trials.h>
#include <immintrin.h>
#include "ConfigManager.h"
#include "Utils.h"

namespace hope{

namespace rtc{

WebRTCManager::WebRTCManager(WebRTCRemoteState state)
    : state(state)
    , connetState(WebRTCConnetState::none)
    , channel(ioContext)
    , accept(ioContext,boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"),19998))
    , tcpSocket(nullptr)
    , ioContextWorkPtr(nullptr)
    , writerChannel(ioContext)
    , msquicSocketClient(nullptr)
    , steadyTimer(ioContext)
{

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this](){
        this->ioContext.run();
    }));

    msquicSocketClient = new hope::quic::MsquicSocketClient(ioContext);

    msquicSocketClient->initialize();

    boost::asio::co_spawn(ioContext,[this]()->boost::asio::awaitable<void>{

        for(;;){

            std::unique_ptr<boost::asio::ip::tcp::socket> socket = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);

            co_await accept.async_accept(*socket,boost::asio::use_awaitable);

            tcpSocket = std::move(socket);

            LOG_INFO("tcpSocket Accept Successful!");

            socketRuns = true;

            followRunning = true;

            wrtierCoroutineAsync();

            receiveCoroutineAysnc();

            std::string registerStr = "{\"requestType\":0,\"webrtcManagerPath\":\"" + ConfigManager::Instance().GetString("WebRTC.WebRTCConfigPath") + "\",\"state\":200}";

            std::shared_ptr<WriterData> registerData = std::make_shared<WriterData>(registerStr.data(), registerStr.size());

            writerAsync(registerData);

            // 发送初始数据
            std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(), dataStr.size());

            writerAsync(writerData);

        }

    },boost::asio::detached);

    systemService = ConfigManager::Instance().GetString("WebRTC.WebRTCService");

    systemServiceExe = ConfigManager::Instance().GetString("WebRTC.WebRTCEXE");
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

    msquicSocketClient->setOnConnectionHandle([this](bool isSuccess){

        if(isSuccess){

            boost::json::object request;

            request["requestType"] = static_cast<int>(WebRTCRequestState::REGISTER);

            request["accountId"] = this->accountId;

            std::string requestStr = boost::json::serialize(request);

            msquicSocketClient->writeJsonAsync(request);

            msquicSocketConnectedHandle(true);

        }else{

            msquicSocketConnectedHandle(false);

            disConnectHandle();

        }

    });

    msquicSocketClient->setOnDataReceivedHandle([this](boost::json::object & json){

        dataStr = boost::json::serialize(json);

        if(this->tcpSocket&& this->state == WebRTCRemoteState::followerRemote && WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::REQUEST){

            std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(),dataStr.size());

            writerAsync(writerData);

            return;
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

                        // 对方是masterRemote(接收端)，我们需要成为followerRemote(发送端)
                        if(remoteState == WebRTCRemoteState::masterRemote){

                            state = WebRTCRemoteState::followerRemote;

                            targetId = std::string(json["accountId"].as_string().c_str());

                            this->followData = dataStr;

                            WindowsServiceManager::stopService(systemService);

                            if(!WindowsServiceManager::serviceExists(systemService)){

                                WindowsServiceManager::registerService(systemService, systemServiceExe);

                            }

                            if (WindowsServiceManager::startService(systemService)) {

                                LOG_INFO("WindowsServiceManager::startService Successful!");

                                boost::asio::co_spawn(ioContext,[this]()mutable->boost::asio::awaitable<void>{

                                    steadyTimer.expires_after(std::chrono::seconds(10));;

                                    co_await steadyTimer.async_wait(boost::asio::use_awaitable);

                                    if (!isRemote) {

                                        releaseSource();

                                        initializePeerConnection();

                                        this->state = WebRTCRemoteState::nullRemote;

                                        isRemote = false;

                                        LOG_INFO("WebRTCManager Offer ReInit");

                                    }


                                },boost::asio::detached);

                                return;
                            }
                        }
                        // 对方是followerRemote(发送端)，我们需要成为masterRemote(接收端)
                        else if(remoteState == WebRTCRemoteState::followerRemote){
                            state = WebRTCRemoteState::masterRemote;
                            targetId = std::string(json["accountId"].as_string().c_str());
                        }
                    }

                    if(json.contains("type")){
                        std::string type(json["type"].as_string().c_str());

                        if(type == "offer"){
                            // 只有当我们不是发起者时才响应offer
                            if(!isInit.load()){
                                if(json.contains("accountId")){
                                    targetId = std::string(json["accountId"].as_string().c_str());
                                }

                                std::string sdp(json["sdp"].as_string().c_str());

                                if(!peerConnection) {
                                    LOG_ERROR("PeerConnection is null, cannot process offer");
                                    return;
                                }

                                // 创建远程描述
                                webrtc::SdpParseError error;
                                std::unique_ptr<webrtc::SessionDescriptionInterface> remoteDesc(
                                    webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error));

                                if(!remoteDesc) {
                                    LOG_ERROR("Failed to parse offer SDP: %s" ,error.description.c_str());
                                    return;
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

                                boost::asio::co_spawn(ioContext,[this]()mutable->boost::asio::awaitable<void>{

                                    steadyTimer.expires_after(std::chrono::seconds(10));;

                                    co_await steadyTimer.async_wait(boost::asio::use_awaitable);

                                    if (!isRemote) {

                                        releaseSource();

                                        initializePeerConnection();

                                        this->state = WebRTCRemoteState::nullRemote;

                                        isRemote = false;

                                        LOG_INFO("WebRTCManager Answer ReInit");

                                    }
                                },boost::asio::detached);


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
                                    LOG_ERROR("Failed to parse ICE candidate: %s" , error.description.c_str());
                                    return;
                                }

                                bool success = peerConnection->AddIceCandidate(candidate.get());
                                if(!success) {
                                    LOG_ERROR("Failed to add ICE candidate");
                                }
                            } else {
                                LOG_ERROR("PeerConnection is null, cannot add ICE candidate");
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

                    if(isRemote == false) return;

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

    });

    msquicSocketClient->connect(host,std::stoi(port));

}

WebRTCManager::~WebRTCManager()
{
    LOG_INFO("Destructing WebRTCManager...");

    // 1. 停止标志位，防止回调继续执行
    socketRuns = false;
    followRunning = false;

    // 2. 停止 Windows 服务
    WindowsServiceManager::stopService(systemService);

    // 3. 停止 MsQuic (防止新消息进来)
    if(msquicSocketClient){
        msquicSocketClient->disconnect();
        delete msquicSocketClient;
        msquicSocketClient = nullptr;
    }

    // 4. 彻底释放 PeerConnection 和相关资源
    releaseSource();

    // 5. [关键] 先释放 Factory，确保它不再使用线程
    // 显式释放，不要依赖默认析构顺序
    peerConnectionFactory = nullptr;

    // 6. 销毁 WebRTC 线程 (只有 Factory 没了，销毁线程才安全)
    if(networkThread){
        networkThread->Quit();
        networkThread.reset();
    }
    if(workerThread){
        workerThread->Quit();
        workerThread.reset();
    }
    if(signalingThread){
        signalingThread->Quit();
        signalingThread.reset();
    }

    // 7. 最后清理 SSL
    webrtc::CleanupSSL();

    // 8. 停止 IO Context
    if (ioContextWorkPtr) {
        ioContextWorkPtr.reset();
    }
    ioContext.stop();
    if(ioContextThread.joinable()){
        ioContextThread.join();
    }

    LOG_INFO("WebRTCManager Destructed.");
}

bool WebRTCManager::initializePeerConnection()
{
    // Clean up any existing connection first
    if (peerConnection) {
        releaseSource();
    }

    if (!peerConnectionFactory) {

        webrtc::InitializeSSL();

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

        const char* field_trials =
            "WebRTC-DataChannelMessageInterleaving/Disabled/"
            "WebRTC-Video-JitterBufferDelay/Enabled/"
            "WebRTC-ZeroPlayoutDelay/min_pacing:2ms/";

        std::unique_ptr<webrtc::FieldTrialsView> fieldTrials = std::make_unique<webrtc::FieldTrials>(field_trials);

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
        LOG_ERROR("Failed to create PeerConnection: %s" ,pcResult.error().message());
        return false;
    }

    peerConnection = pcResult.MoveValue();

    return true;
}

void WebRTCManager::writerAsync(std::shared_ptr<WriterData> writerData){
    writerDataQueues.enqueue(writerData);
    if (writerChannel.is_open()) {
        writerChannel.try_send(boost::system::error_code{});
    }
}

void WebRTCManager::disConnectRemote()
{

    SystemParametersInfo(SPI_SETCURSORS,0,NULL,0);

    this->state = WebRTCRemoteState::nullRemote;

    if(isRemote == false) return;

    isRemote = false;

    releaseSource();

    initializePeerConnection();

    WindowsServiceManager::stopService(systemService);

    if(msquicSocketClient && msquicSocketClient->isConnected()){

        boost::json::object message;

        message["accountId"] = this->accountId;

        message["targetId"] = this->targetId;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);

        msquicSocketClient->writeJsonAsync(message);
    }

}

void WebRTCManager::disConnectHandle()
{
    SystemParametersInfo(SPI_SETCURSORS,0,NULL,0);

    this->state = WebRTCRemoteState::nullRemote;

    isRemote = false;

    if(disConnectRemoteHandle){

        disConnectRemoteHandle();

    }

    releaseSource();

    WindowsServiceManager::stopService(systemService);

}

void WebRTCManager::setSystemServiceExe(std::string webrtcExe)
{
    this->systemServiceExe = webrtcExe;
}

void WebRTCManager::handleCursor(const unsigned char *data, size_t size)
{
    // Use thread-local storage to avoid thread safety issues
    static thread_local HCURSOR lastCursor = nullptr;

    // Minimum size check
    if (size < sizeof(short)) {
        LOG_ERROR("Message too small to contain type");
        return;
    }

    short type = -1;
    memcpy(&type, data, sizeof(short));

#pragma pack(push, 1)
    struct CursorMessage {
        short type;
        int index;
        int width;
        int height;
        int hotX;
        int hotY;
    };
#pragma pack(pop)

    switch(type) {
    case 0: { // Cursor index message
        if (size < sizeof(CursorMessage)) {
            LOG_ERROR("Invalid cursor index message size");
            break;
        }

        const CursorMessage* msg = reinterpret_cast<const CursorMessage*>(data);

        // CRITICAL: Validate index bounds
        if (msg->index < 0 || msg->index >= this->cursorArray.size()) {
            LOG_ERROR("Invalid cursor index: %d (array size: %zu)", msg->index, this->cursorArray.size());
            break;
        }

        // Validate dimensions
        if (msg->width <= 0 || msg->width > 256 ||
            msg->height <= 0 || msg->height > 256) {
            LOG_ERROR("Invalid cursor dimensions: %dx%d", msg->width, msg->height);
            break;
        }

        // Get cursor data
        std::vector<unsigned char>& cursorData = this->cursorArray[msg->index];

        // Verify stored data size matches expected size
        size_t expectedSize = msg->width * msg->height * 4; // RGBA
        if (cursorData.size() != expectedSize) {
            LOG_ERROR("Stored cursor data size mismatch. Expected: %zu, Got: %zu", expectedSize, cursorData.size());
            break;
        }

        // Create cursor
        HCURSOR cursor = CreateCursorFromRGBA(cursorData.data(), msg->width,
                                              msg->height, msg->hotX, msg->hotY);
        if (cursor) {
            // Clean up previous cursor
            if (lastCursor) {
                DestroyCursor(lastCursor);
            }
            lastCursor = CopyCursor(cursor);
            SetSystemCursor(lastCursor, 32512);
            DestroyCursor(cursor); // Clean up the temporary cursor
        }
        break;
    }

    case 1: { // New cursor data
        if (size < sizeof(CursorMessage)) {
            LOG_ERROR("Invalid new cursor message size");
            break;
        }

        const CursorMessage* msg = reinterpret_cast<const CursorMessage*>(data);

        // Validate dimensions
        if (msg->width <= 0 || msg->width > 256 ||
            msg->height <= 0 || msg->height > 256) {
            LOG_ERROR("Invalid cursor dimensions: %dx%d", msg->width, msg->height);
            break;
        }

        // Validate index
        if (msg->index < 0 || msg->index > this->cursorArray.size()) {
            LOG_ERROR("Invalid cursor index for storage: %d", msg->index);
            break;
        }

        // Calculate image data size
        size_t headerSize = sizeof(CursorMessage);

        // Prevent integer underflow
        if (size <= headerSize) {
            LOG_ERROR("No cursor image data");
            break;
        }

        size_t imageSize = size - headerSize;

        // Verify image data size
        size_t expectedSize = msg->width * msg->height * 4; // RGBA
        if (imageSize != expectedSize) {
            LOG_ERROR("Image data size mismatch. Expected: %zu, Got: %zu", expectedSize, imageSize);
            break;
        }

        // Store cursor data
        std::vector<unsigned char> cursorData(imageSize);
        memcpy(cursorData.data(), data + headerSize, imageSize);

        // Add or update cursor in array
        if (msg->index == this->cursorArray.size()) {
            this->cursorArray.push_back(std::move(cursorData));
        } else {
            this->cursorArray[msg->index] = std::move(cursorData);
        }

        // Create cursor
        HCURSOR cursor = CreateCursorFromRGBA(this->cursorArray[msg->index].data(),
                                              msg->width, msg->height,
                                              msg->hotX, msg->hotY);
        if (cursor) {
            // Clean up previous cursor
            if (lastCursor) {
                DestroyCursor(lastCursor);
            }
            lastCursor = CopyCursor(cursor);
            SetSystemCursor(lastCursor, 32512);
            DestroyCursor(cursor); // Clean up the temporary cursor
        }
        break;
    }

    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }
}

void WebRTCManager::wrtierCoroutineAsync()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        try {
            for (;;) {

                std::shared_ptr<WriterData> nowNode = nullptr;
                while (this->writerDataQueues.try_dequeue(nowNode) && nowNode != nullptr) {
                    try {

                        co_await boost::asio::async_write(*this->tcpSocket,
                                                          boost::asio::buffer(nowNode->data, nowNode->size),
                                                          boost::asio::use_awaitable);

                    } catch (const boost::system::system_error& e) {
                        // 检查特定错误
                        if (e.code() == boost::asio::error::broken_pipe ||
                            e.code() == boost::asio::error::connection_reset ||
                            e.code() == boost::asio::error::connection_aborted) {
                            LOG_ERROR("Connection lost, exiting writer coroutine");
                            co_return;
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Unexpected error sending data: %s" , e.what());
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
            LOG_ERROR("Writer coroutine unhandled exception: %s",e.what());
        } catch (...) {
            LOG_ERROR("Writer coroutine unknown exception");
        }
        co_return;
    }, [this](std::exception_ptr p) {
                              try {
                                  if (p) {
                                      std::rethrow_exception(p);
                                  }
                              } catch (const std::exception& e) {
                                  LOG_ERROR("Writer coroutine exception: %s",e.what());
                              }
                          });
}

void WebRTCManager::receiveCoroutineAysnc()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        char headerBuffer[8];
        size_t headerSize = sizeof(int64_t);
        int messageCount = 0;

        while (socketRuns) {
            std::memset(headerBuffer, 0, headerSize);

            // 接收消息头
            size_t headerRead = 0;
            while (headerRead < headerSize) {
                size_t n = co_await this->tcpSocket->async_read_some(
                    boost::asio::buffer(headerBuffer + headerRead, headerSize - headerRead),
                    boost::asio::use_awaitable);

                if (n == 0) {
                    co_return;
                }
                headerRead += n;
            }

            int64_t rawBodyLength = 0;
            std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
            int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

            if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) { // 限制最大10MB
                LOG_ERROR("Invalid body length: %d" ,bodyLength);
                co_return;
            }

            size_t bodySize = static_cast<size_t>(bodyLength);

            // 使用智能指针管理内存
            std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]); // +1 for null terminator
            if (!bodyBuffer) {
                LOG_ERROR("Failed to allocate memory for body buffer");
                co_return;
            }
            std::memset(bodyBuffer.get(), 0, bodySize + 1);

            // 接收消息体
            size_t bodyRead = 0;
            while (bodyRead < bodySize) {
                size_t n = co_await this->tcpSocket->async_read_some(
                    boost::asio::buffer(bodyBuffer.get() + bodyRead, bodySize - bodyRead),
                    boost::asio::use_awaitable);

                if (n == 0) {
                    co_return;
                }
                bodyRead += n;
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

            if(msquicSocketClient && msquicSocketClient->isConnected()){
                msquicSocketClient->writeJsonAsync(json);
            }
        }

        co_return;
    }, [this](std::exception_ptr p) {
                              try {
                                  if (p) {
                                      std::rethrow_exception(p);
                                  }
                              }
                              catch (const std::exception& e) {
                                  handleAsioException();
                                  LOG_ERROR("Reader coroutine handler exception: %s", e.what());
                              }
                          });
}

void WebRTCManager::sendSignalingMessage(boost::json::object& msg) {

    if (!msquicSocketClient || !msquicSocketClient->isConnected()) {
        LOG_ERROR("Cannot send signaling message - WebSocket not connected");
        return;
    }

    // 添加发送者信息
    msg["accountId"] = accountId;
    if (!targetId.empty()) {
        msg["targetId"] = targetId;
    }

    msg["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
    msg["state"] = 200;

    try {
        msquicSocketClient->writeJsonAsync(msg);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to send signaling message: %s",e.what());
    }
}

void WebRTCManager::handleAsioException()
{

    this->state = WebRTCRemoteState::nullRemote;

    isRemote = false;

    if(msquicSocketClient && msquicSocketClient->isConnected()){

        boost::json::object message;

        message["accountId"] = this->accountId;

        message["targetId"] = this->targetId;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::RESTART);

        msquicSocketClient->writeJsonAsync(message);

        WindowsServiceManager::stopService(systemService);


    }
}

void WebRTCManager::releaseSource()
{
    // 1. 先关闭 PeerConnection (阻塞调用，确保信令停止)
    if (peerConnection) {
        peerConnection->Close();
    }

    // 2. 停止数据通道
    if (dataChannel) {
        dataChannel->Close();
        dataChannel = nullptr;
    }

    // 3. 释放 Track 和 Sender
    if (videoTrack && videoSinkImpl) {
        auto vt = static_cast<webrtc::VideoTrackInterface*>(videoTrack.get());
        vt->RemoveSink(videoSinkImpl.get());
        videoSinkImpl.reset();
    }
    videoSender = nullptr;
    videoTrack = nullptr;
    audioSender = nullptr;
    audioTrack = nullptr;

    // 4. [关键] 先销毁 PeerConnection，再销毁 Observer
    // 这样 PC 销毁时如果想回调 Observer，Observer 还在
    peerConnection = nullptr;

    // 5. 现在可以安全销毁 Observer 了
    peerConnectionObserver.reset();
    dataChannelObserver.reset();
    createOfferObserver = nullptr;
    createAnswerObserver = nullptr;

    // 6. 清理状态
    isInit = false;

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

std::string WebRTCManager::getAccountId() const
{
    return accountId;
}

void WebRTCManager::setAccountId(const std::string &newAccountId)
{
    accountId = newAccountId;
}

void WebRTCManager::sendRequestToTarget(int webrtcModulesType,int webrtcUseLevels,int videoCodec,int webrtcAudioEnable)
{

    if(peerConnection == nullptr){

        initializePeerConnection();

    }

    boost::asio::co_spawn(ioContext,[=]()->boost::asio::awaitable<void>{
        if (targetId.empty()) {
            LOG_ERROR("Target ID not set");
            co_return;
        }

        if (!msquicSocketClient || !msquicSocketClient->isConnected()) {
            LOG_ERROR("WebSocket not connected");
            co_return;
        }

        if(peerConnection != nullptr){
            // 避免重复设置状态
            if (state.load() != WebRTCRemoteState::nullRemote) {
                LOG_WARNING("State already set, current state: %d", static_cast<int>(state.load()));
                co_return;
            }

            boost::json::object message;
            message["accountId"] = accountId;
            message["targetId"] = targetId;
            message["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
            message["webRTCRemoteState"] = static_cast<int64_t>(WebRTCRemoteState::masterRemote);
            message["webrtcModulesType"] = webrtcModulesType;
            message["webrtcUseLevels"] = webrtcUseLevels;
            message["codec"] = videoCodec;
            message["webrtcAudioEnable"] = webrtcAudioEnable;

            msquicSocketClient->writeJsonAsync(message);

            LOG_INFO("Request sent to target: %s", targetId.c_str());
        }

    },boost::asio::detached);

}

std::string WebRTCManager::getTargetId() const
{
    return targetId;
}

void WebRTCManager::setTargetId(const std::string &newTargetId)
{
    targetId = newTargetId;
}

void WebRTCManager::writerRemote(unsigned char *data, size_t size)
{
    if(state.load() == WebRTCRemoteState::masterRemote){

        if(!dataChannel) {

            LOG_ERROR("DataChannel is null");

            delete reinterpret_cast<void*>(data);

            return;
        }

        webrtc::CopyOnWriteBuffer buffer(data, size);

        webrtc::DataBuffer dataBuffer(buffer, true); // true 表示二进制数据

        dataChannel->SendAsync(dataBuffer,[this,data](webrtc::RTCError){

            delete reinterpret_cast<void*>(data);

        });

        return;

    }
}

void WebRTCManager::setVideoFrameCallback(VideoFrameCallback callback)
{
    this->videoFrameCallback = callback;
}

void WebRTCManager::disConnect()
{

    boost::asio::post(ioContext,[this](){

        if (msquicSocketClient && msquicSocketClient->isConnected()) {

            msquicSocketClient->disconnect();
        }

        if(state == WebRTCRemoteState::followerRemote){
            socketRuns = false;
            followRunning = false;

            if(tcpSocket && tcpSocket->is_open()){

                tcpSocket->close();
            }

            WindowsServiceManager::stopService(systemService);
        }

        this->state = WebRTCRemoteState::nullRemote;

        disConnectHandle();

        isRemote = false;

    });

}


}

}
