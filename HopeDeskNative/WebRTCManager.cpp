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

#ifdef _WIN32
#include <winsock2.h>      // Windows Socket API
#include <ws2tcpip.h>      // Windows Socket 扩展
#include <mstcpip.h>       // SIO_KEEPALIVE_VALS 和 tcp_keepalive 结构体
#pragma comment(lib, "ws2_32.lib")
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace hope{

namespace rtc{

WebRTCManager::WebRTCManager()
    : accept(ioContext,boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"),19998))
    , tcpSocket(nullptr)
    , ioContextWorkPtr(nullptr)
    , webSocket(nullptr)
    , asioConcurrentQueue(ioContext.get_executor())
    , webrtcAsioConcurrentQueue(ioContext.get_executor())
    , reloadTimer(ioContext)
    , peerConnection(nullptr)
{

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this](){
        this->ioContext.run();
    }));

    sslContext.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use
        );

    systemService = ConfigManager::Instance().GetString("WebRTC.WebRTCService");

    systemServiceExe = ConfigManager::Instance().GetString("WebRTC.WebRTCEXE");

}

void WebRTCManager::asyncEvent(){

    if(asyncAccpets.exchange(true)) return;

    boost::asio::co_spawn(ioContext,[self = shared_from_this()]()->boost::asio::awaitable<void>{

        while(self->asyncAccpets.load()){

            std::unique_ptr<boost::asio::ip::tcp::socket> socket = std::make_unique<boost::asio::ip::tcp::socket>(self->ioContext);

            co_await self->accept.async_accept(*socket,boost::asio::use_awaitable);

            self->tcpSocket = std::move(socket);

            LOG_INFO("tcpSocket Accept Successful!");

            self->asyncEvents = true;

            self->followRunning = true;

            self->asioConcurrentQueue.reset();

            self->receiveCoroutineAysnc();

            boost::asio::co_spawn(self->ioContext,self->writerCoroutineAsync(),boost::asio::detached);

            std::string registerStr = "{\"requestType\":0,\"webrtcManagerPath\":\"" + ConfigManager::Instance().GetString("WebRTC.WebRTCConfigPath") + "\",\"state\":200}";

            std::shared_ptr<WriterData> registerData = std::make_shared<WriterData>(registerStr.data(), registerStr.size());

            self->asyncWrite(registerData);

            // 发送初始数据
            std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(self->dataStr.data(), self->dataStr.size());

            self->asyncWrite(writerData);

        }

    },boost::asio::detached);

}

void WebRTCManager::closeEvent(){

    if(!asyncAccpets.exchange(false)) return;

}

void WebRTCManager::connect(std::string ip)
{
    std::string host = ip;
    std::string port = "443"; // 默认端口，你可以根据需要修改

    // 如果 IP 包含端口号（格式：ip:port）
    size_t colonPos = ip.find(':');
    if (colonPos != std::string::npos) {
        host = ip.substr(0, colonPos);
        port = ip.substr(colonPos + 1);
    }

    boost::asio::co_spawn(ioContext, [self = shared_from_this(),host,port]()mutable->boost::asio::awaitable<void> {

        try {

            self->webSocket = std::make_unique<boost::beast::websocket::stream<
                boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>>(self->ioContext, self->sslContext);

            boost::asio::ip::tcp::resolver resolver(self->ioContext);

            // 1. DNS 解析（带超时）
            auto results = co_await resolver.async_resolve(
                host, port,
                boost::asio::cancel_after(RESOLVE_TIMEOUT, boost::asio::use_awaitable)
                );

            // 2. TCP 连接（带超时）
            co_await boost::asio::async_connect(
                self->webSocket->next_layer().next_layer(),
                results,
                boost::asio::cancel_after(CONNECT_TIMEOUT, boost::asio::use_awaitable)
                );

            // 3. SSL 握手（带超时）
            co_await self->webSocket->next_layer().async_handshake(
                boost::asio::ssl::stream_base::client,
                boost::asio::cancel_after(SSL_HANDSHAKE_TIMEOUT, boost::asio::use_awaitable)
                );

            // 4. WebSocket 握手（带超时）
            co_await self->webSocket->async_handshake(
                host, "/",
                boost::asio::cancel_after(WS_HANDSHAKE_TIMEOUT, boost::asio::use_awaitable)
                );

            self->webrtcAsioConcurrentQueue.reset();

            self->setTcpKeepAlive(self->webSocket->next_layer().next_layer());

            self->webrtcAsyncEvents.store(true);

            boost::asio::co_spawn(self->ioContext, self->webrtcReceiveCoroutine(), boost::asio::detached);

            boost::asio::co_spawn(self->ioContext, self->webrtcWriteCoroutine(), boost::asio::detached);

            boost::json::object request;

            request["requestType"] = static_cast<int>(WebRTCRequestState::REGISTER);

            request["accountId"] = self->accountId;

            std::string requestStr = boost::json::serialize(request);

            self->webrtcAsyncWrite(requestStr);

        }
        catch (std::exception & e) {

            LOG_ERROR("WebSocket Connect Error : %s",e.what());

            self->webrtcAsyncEvents.store(true);

            self->closeWebSocket();

            if (self->onSignalServerDisConnectHandle) {
                self->onSignalServerDisConnectHandle();
            }

            if (self->isRemote == false) {

                co_return;

            }

            self->isRemote = false;

            if (self->onDisConnectRemoteHandle) {

                self->onDisConnectRemoteHandle();

            }

            self->releaseSource();

            self->initializePeerConnection();

            co_return;
        }

    },boost::asio::detached);


}

WebRTCManager::~WebRTCManager()
{
    LOG_INFO("Destructing WebRTCManager...");

    closeEvent();

    onSignalServerDisConnectHandle = nullptr;
    onFollowRemoteHandle = nullptr;
    onDisConnectRemoteHandle = nullptr;
    onRemoteSuccessFulHandle = nullptr;
    onSignalServerConnectHandle = nullptr;
    onRemoteFailedHandle = nullptr;
    onResetCursorHandle = nullptr;
    onRTCStatsCollectorHandle = nullptr;

    asyncEvents = false;
    followRunning = false;

    WindowsServiceManager::stopService(systemService);

    if(webSocket){
        closeWebSocket();
        webSocket = nullptr;
    }

    releaseSource();

    peerConnectionFactory = nullptr;

    webrtcVideoEncoderFactory = nullptr;

    webrtcVideoDecoderFactory = nullptr;

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

    webrtc::CleanupSSL();

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

        std::unique_ptr<WebRTCVideoEncoderFactory> webrtcVideoEncoderFactoryUnique = std::make_unique<WebRTCVideoEncoderFactory>();

        webrtcVideoEncoderFactory = webrtcVideoEncoderFactoryUnique.get();

        std::unique_ptr<WebRTCVideoDecoderFactory> webrtcVideoDecoderFactoryUnique = std::make_unique<WebRTCVideoDecoderFactory>();

        webrtcVideoDecoderFactory = webrtcVideoDecoderFactoryUnique.get();

        peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
            networkThread.get(),
            workerThread.get(),
            signalingThread.get(),
            nullptr,
            webrtc::CreateBuiltinAudioEncoderFactory(),
            webrtc::CreateBuiltinAudioDecoderFactory(),
            std::move(webrtcVideoEncoderFactoryUnique),
            std::move(webrtcVideoDecoderFactoryUnique),
            nullptr,
            nullptr,
            nullptr,
            nullptr
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

    webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>>  peerConnectionResult = peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pcDependencies));

    if (!peerConnectionResult.ok()) {
        LOG_ERROR("Failed to create PeerConnection: %s" ,peerConnectionResult.error().message());
        return false;
    }

    peerConnection = peerConnectionResult.MoveValue();

    return true;
}

void WebRTCManager::asyncWrite(std::shared_ptr<WriterData> writerData){
    asioConcurrentQueue.enqueue(std::move(writerData));
}

void WebRTCManager::webrtcAsyncWrite(std::string str)
{
    webrtcAsioConcurrentQueue.enqueue(std::move(str));
}

void WebRTCManager::disConnectRemote()
{

    if(onResetCursorHandle) onResetCursorHandle();

    if(isRemote == false) return;

    isRemote = false;

    releaseSource();

    initializePeerConnection();

    if(webSocket && webSocket->is_open()){

        boost::json::object message;

        message["accountId"] = this->accountId;

        message["targetId"] = this->targetId;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);

        webrtcAsyncWrite(boost::json::serialize(message));
    }

}

void WebRTCManager::disConnectRemoteHandler()
{

    if(onResetCursorHandle) onResetCursorHandle();

    if(isRemote == false) return;

    isRemote = false;

    if(tcpSocket){

        asyncEvents = false;

        followRunning = false;

        asioConcurrentQueue.close();

        if(tcpSocket && tcpSocket->is_open()){

            tcpSocket->close();
        }

        tcpSocket = nullptr;
    }

    releaseSource();

    initializePeerConnection();

    if(onDisConnectRemoteHandle){

        onDisConnectRemoteHandle();

    }

}

void WebRTCManager::closeWebSocket()
{

    if(!webrtcAsyncEvents.exchange(false)) return;

    boost::system::error_code ec;

    webrtcAsioConcurrentQueue.close();

    // 取消底层 TCP socket
    auto& tcpSocket = webSocket->next_layer().next_layer();
    tcpSocket.cancel(ec);
    if (ec) {
        LOG_ERROR("WebRTCManager::closeSocket() can't cancel Socket: %s", ec.message().c_str());
    }
    // WebSocket 关闭帧
    if (webSocket->is_open()) {
        try {
            webSocket->close(boost::beast::websocket::close_code::normal, ec);
        }
        catch (const std::exception& e) {
            LOG_ERROR("WebRTCManager::closeSocket() close WebSocket failed: %s", e.what());
        }
    }

    // SSL 关闭
    if (webSocket->next_layer().next_layer().is_open()) {
        webSocket->next_layer().shutdown(ec);
        webSocket->next_layer().next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        webSocket->next_layer().next_layer().close(ec);
        if (ec && ec != boost::asio::error::not_connected) {
            LOG_ERROR("WebRTCManager::closeSocket() close Tcp Socket failed: %s", ec.message().c_str());
        }
    }

    webSocket = nullptr;

    LOG_INFO("WebRTCManager::WebSocket is close");
}

void WebRTCManager::setTcpKeepAlive(boost::asio::ip::tcp::socket &sock, int idle, int intvl, int probes)
{
    int fd = sock.native_handle();

    /* 1. 先打开 SO_KEEPALIVE 通用开关 */
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&on), sizeof(on));

#if defined(__linux__)
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));

#elif defined(_WIN32)
    /* Windows 用毫秒结构体 */
    struct tcp_keepalive kalive {};
    kalive.onoff = 1;
    kalive.keepalivetime = idle * 1000;   // ms
    kalive.keepaliveinterval = intvl * 1000;   // ms
    DWORD bytes_returned = 0;
    WSAIoctl(fd, SIO_KEEPALIVE_VALS,
             &kalive, sizeof(kalive),
             nullptr, 0, &bytes_returned, nullptr, nullptr);

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    /* macOS / BSD 用秒级 TCP_KEEPALIVE 等选项 */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));   // 同 Linux 的 IDLE
    /* 间隔与次数在 BSD 上只有一个 TCP_KEEPINTVL，单位秒 */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    /* BSD 没有 KEEPCNT，用 TCP_KEEPALIVE 的初始值+间隔推算，效果相近 */
#else
#warning "Unsupported platform, TCP keep-alive parameters not tuned"
#endif
}

boost::asio::awaitable<void> WebRTCManager::webrtcReceiveCoroutine()
{

    try{

        while (webrtcAsyncEvents.load()) {

            boost::beast::flat_buffer buffer;

            co_await webSocket->async_read(buffer, boost::asio::use_awaitable);

            std::string str = boost::beast::buffers_to_string(buffer.data());

            buffer.consume(buffer.size());

            boost::json::object json;

            try {
                json = boost::json::parse(str).as_object();
            }
            catch (const std::exception& e) {
                LOG_ERROR("WebSocket received invalid JSON: %s", e.what());
                continue;
            }

            dataStr = boost::json::serialize(json);

            if(this->tcpSocket && WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::REQUEST){

                std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(),dataStr.size());

                asyncWrite(writerData);

                continue;
            }

            if(json.contains("requestType")){
                int64_t requestType = json["requestType"].as_int64();

                int64_t responseState = json["state"].as_int64();

                if(WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER){

                    if(responseState == 200){

                        if(onSignalServerConnectHandle){

                            onSignalServerConnectHandle();

                        }

                        if(!peerConnection){

                            initializePeerConnection();

                        }

                    }
                }else if(WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST){

                    if(responseState == 200){

                        if(json.contains("webRTCRemoteState")){

                            WebRTCRemoteState remoteState = WebRTCRemoteState(json["webRTCRemoteState"].as_int64());

                            if(remoteState == WebRTCRemoteState::masterRemote){

                                targetId = std::string(json["accountId"].as_string().c_str());

                                this->followData = dataStr;

                                WindowsServiceManager::stopService(systemService);

                                if(!WindowsServiceManager::serviceExists(systemService)){

                                    WindowsServiceManager::registerService(systemService, systemServiceExe);

                                }

                                if (WindowsServiceManager::startService(systemService)) {

                                    LOG_INFO("WindowsServiceManager::startService Successful!");

                                    boost::asio::co_spawn(ioContext,[this]()mutable->boost::asio::awaitable<void>{

                                        reloadTimer.expires_after(std::chrono::seconds(15));;

                                        co_await reloadTimer.async_wait(boost::asio::use_awaitable);

                                        if (!isRemote) {

                                            if(tcpSocket){

                                                asyncEvents = false;

                                                followRunning = false;

                                                asioConcurrentQueue.close();

                                                if(tcpSocket && tcpSocket->is_open()){

                                                    tcpSocket->close();
                                                }

                                                tcpSocket = nullptr;
                                            }

                                            releaseSource();

                                            initializePeerConnection();

                                            isRemote = false;

                                            LOG_INFO("WebRTCManager Offer ReInit");

                                        }


                                    },boost::asio::detached);

                                    continue;
                                }
                            }
                            // 对方是followerRemote(发送端)，我们需要成为masterRemote(接收端)
                            else if(remoteState == WebRTCRemoteState::followerRemote){
                                targetId = std::string(json["accountId"].as_string().c_str());
                            }
                        }

                        if(json.contains("type")){
                            std::string type(json["type"].as_string().c_str());

                            if(type == "offer"){
                                if(json.contains("accountId")){
                                    targetId = std::string(json["accountId"].as_string().c_str());
                                }

                                std::string sdp(json["sdp"].as_string().c_str());

                                if(!peerConnection) {
                                    LOG_ERROR("PeerConnection is null, cannot process offer");
                                    continue;
                                }

                                // 创建远程描述
                                webrtc::SdpParseError error;
                                std::unique_ptr<webrtc::SessionDescriptionInterface> remoteDesc(
                                    webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error));

                                if(!remoteDesc) {
                                    LOG_ERROR("Failed to parse offer SDP: %s" ,error.description.c_str());
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
                                        continue;
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

                        if(onRemoteFailedHandle){

                            onRemoteFailedHandle();

                        }

                    }

                }else if(WebRTCRequestState(requestType) == WebRTCRequestState::RESTART){

                    if(responseState == 200){

                        if(isRemote == false) {

                            boost::json::object request;

                            request["requestType"] = static_cast<int>(WebRTCRequestState::CLOSESYSTEM);

                            request["accountId"] = accountId;

                            request["targetId"] = targetId;

                            webrtcAsyncWrite(boost::json::serialize(request));

                            continue;

                        }

                        releaseSource();

                        initializePeerConnection();

                        asyncReomteDesk();
                    }
                }else if(WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE){

                    if(responseState == 200){

                        disConnectRemoteHandler();

                    }

                }else if(WebRTCRequestState(requestType) == WebRTCRequestState::CLOSESYSTEM){

                    if(responseState == 200){

                        if(tcpSocket){

                            asyncEvents = false;

                            followRunning = false;

                            asioConcurrentQueue.close();

                            if(tcpSocket && tcpSocket->is_open()){

                                tcpSocket->close();
                            }

                            tcpSocket = nullptr;
                        }

                        WindowsServiceManager::stopService(systemService);

                        targetId = json["accountId"].as_string().c_str();

                        boost::json::object request;

                        request["requestType"] = static_cast<int>(WebRTCRequestState::SYSTEMREADLY);

                        request["accountId"] = accountId;

                        request["targetId"] = targetId;

                        webrtcAsyncWrite(boost::json::serialize(request));

                    }

                }else if(WebRTCRequestState(requestType) == WebRTCRequestState::SYSTEMREADLY){

                    if(responseState == 200){

                        asyncReomteDesk();

                    }

                }
            }


        }

    }catch(std::exception & e){

        LOG_ERROR("WebSocket Connect Error : %s",e.what());

        webrtcAsyncEvents.store(true);

        closeWebSocket();

        if (onSignalServerDisConnectHandle) {
            onSignalServerDisConnectHandle();
        }

        if (isRemote == false) {


            co_return;

        }

        isRemote = false;

        if (onDisConnectRemoteHandle) {

            onDisConnectRemoteHandle();

        }

        releaseSource();

        initializePeerConnection();

        co_return;

    }

}

boost::asio::awaitable<void> WebRTCManager::webrtcWriteCoroutine()
{
    try {

        while (webrtcAsyncEvents.load()) {

            std::optional<std::string> optional = co_await webrtcAsioConcurrentQueue.dequeue();

            if (optional.has_value()) {

                std::string str = std::move(optional.value());

                co_await webSocket->async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

            }else break;

            if (!webrtcAsyncEvents.load()) break;

        }

    }
    catch (const std::exception& e) {

        LOG_ERROR("Writer coroutine unhandled exception: %s", e.what());

    }
    catch (...) {

        LOG_ERROR("Writer coroutine unknown exception");

    }
    co_return;
}

void WebRTCManager::disConnectHandle()
{

    if(onResetCursorHandle) onResetCursorHandle();

    if(tcpSocket){

        asyncEvents = false;

        followRunning = false;

        asioConcurrentQueue.close();

        if(tcpSocket && tcpSocket->is_open()){

            tcpSocket->close();
        }

        tcpSocket = nullptr;
    }

    releaseSource();

    initializePeerConnection();

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
        LOG_WARN("Unknown message type: %d", type);
        break;
    }
}

boost::asio::awaitable<void> WebRTCManager::writerCoroutineAsync()
{
    try {

        while(asyncEvents.load()){

            std::optional<std::shared_ptr<WriterData>> optional = co_await asioConcurrentQueue.dequeue();

            if(optional.has_value()){

                std::shared_ptr<WriterData> writeData = optional.value();

                co_await boost::asio::async_write(*tcpSocket,boost::asio::buffer(writeData->data,writeData->size),boost::asio::use_awaitable);

            }else break;

            if (!asyncEvents.load()) break;

        }

    } catch (const std::exception& e) {

        LOG_ERROR("Writer coroutine unhandled exception: %s",e.what());

    } catch (...) {

        LOG_ERROR("Writer coroutine unknown exception");

    }
    co_return;
}

void WebRTCManager::receiveCoroutineAysnc()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {
        char headerBuffer[8];
        size_t headerSize = sizeof(int64_t);
        int messageCount = 0;

        while (asyncEvents) {
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

                this->onFollowRemoteHandle();

                this->isRemote = true;

                continue;

            }else if(WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::CLOSE){

                disConnectRemoteHandler();

                continue;
            }else if(WebRTCRequestState(json["requestType"].as_int64()) == WebRTCRequestState::STATS){

                int type = json["connectionType"].as_int64();

                if(onRTCStatsCollectorHandle){

                    onRTCStatsCollectorHandle(type);

                }

            }

            if(webSocket && webSocket->is_open()){
                webrtcAsyncWrite(boost::json::serialize(json));
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

    if (!webSocket || !webSocket->is_open()) {
        LOG_ERROR("Cannot send signaling message - WebSocket not connected");
        if(onSignalServerDisConnectHandle){

            onSignalServerDisConnectHandle();

        }
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
        webrtcAsyncWrite(boost::json::serialize(msg));
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to send signaling message: %s",e.what());
    }
}

void WebRTCManager::handleAsioException()
{

    if(!isRemote.exchange(false)) return;

    if(tcpSocket){

        asyncEvents = false;

        followRunning = false;

        asioConcurrentQueue.close();

        if(tcpSocket && tcpSocket->is_open()){

            tcpSocket->close();
        }

        tcpSocket = nullptr;
    }

    if(webSocket && webSocket->is_open()){

        boost::json::object message;

        message["accountId"] = this->accountId;

        message["targetId"] = this->targetId;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::RESTART);

        webrtcAsyncWrite(boost::json::serialize(message));

        WindowsServiceManager::stopService(systemService);

    }
}

void WebRTCManager::releaseSource()
{

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

    peerConnection = nullptr;

    // 5. 现在可以安全销毁 Observer 了
    peerConnectionObserver.reset();
    dataChannelObserver.reset();
    createOfferObserver = nullptr;
    createAnswerObserver = nullptr;
    rtcStatsCollectorHandle = nullptr;

    if(tcpSocket){

        asyncEvents = false;

        followRunning = false;

        asioConcurrentQueue.close();

        if(tcpSocket && tcpSocket->is_open()){

            tcpSocket->close();
        }

        tcpSocket = nullptr;
    }

    WindowsServiceManager::stopService(systemService);  // ← 也可能在这里阻塞

}

std::string WebRTCManager::getAccountId() const
{
    return accountId;
}

void WebRTCManager::setAccountId(const std::string &newAccountId)
{
    accountId = newAccountId;
}

void WebRTCManager::asyncReomteDesk(int webrtcModulesType,int webrtcUseLevels,int videoCodec,int webrtcAudioEnable,int webrtcEnableNvidia)
{
    boost::asio::co_spawn(ioContext,[=,self = shared_from_this()]()->boost::asio::awaitable<void>{

        if(self->peerConnection == nullptr){

            self->initializePeerConnection();

        }

        if (targetId.empty()) {
            LOG_ERROR("Target ID not set");
            co_return;
        }

        if (!self->webSocket || !self->webSocket->is_open()) {
            LOG_ERROR("WebSocket not connected");
            co_return;
        }

        if(self->peerConnection != nullptr){

            boost::json::object message;
            message["accountId"] = self->accountId;
            message["targetId"] = self->targetId;
            message["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
            message["webRTCRemoteState"] = static_cast<int64_t>(WebRTCRemoteState::masterRemote);
            message["webrtcModulesType"] = webrtcModulesType;
            message["webrtcUseLevels"] = webrtcUseLevels;
            message["codec"] = videoCodec;
            message["webrtcAudioEnable"] = webrtcAudioEnable;
            message["webrtcEnableNvidia"] = webrtcEnableNvidia;

            self->webrtcAsyncWrite(boost::json::serialize(message));

            LOG_INFO("Request sent to target: %s", targetId.c_str());

            boost::asio::co_spawn(ioContext,[self = self->shared_from_this()]()mutable->boost::asio::awaitable<void>{

                self->reloadTimer.expires_after(std::chrono::seconds(15));;

                co_await self->reloadTimer.async_wait(boost::asio::use_awaitable);

                if (!self->isRemote) {

                    if(self->tcpSocket){

                        self->asyncEvents = false;

                        self->followRunning = false;

                        self->asioConcurrentQueue.close();

                        if(self->tcpSocket && self->tcpSocket->is_open()){

                            self->tcpSocket->close();
                        }

                        self->tcpSocket = nullptr;
                    }

                    self->releaseSource();

                    self->initializePeerConnection();

                    self->isRemote = false;

                    LOG_INFO("WebRTCManager AsyncReomteDesk ReInit");

                }
            },boost::asio::detached);
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

void WebRTCManager::setOnVideoFrameHanlder(std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHandler)
{
    this->onVideoFrameHandler = onVideoFrameHandler;
}

void WebRTCManager::disConnect()
{

    boost::asio::post(ioContext,[self = shared_from_this()](){

        if (self->webSocket && self->webSocket->is_open()) {

            self->closeWebSocket();

            self->webSocket = nullptr;
        }

        if(self->tcpSocket){

            self->asyncEvents = false;

            self->followRunning = false;

            self->asioConcurrentQueue.close();

            if(self->tcpSocket && self->tcpSocket->is_open()){

                self->tcpSocket->close();

            }

            self->tcpSocket = nullptr;
        }

        if(self->onDisConnectRemoteHandle){

            self->onDisConnectRemoteHandle();

        }

        self->disConnectHandle();

    });

}


}

}
