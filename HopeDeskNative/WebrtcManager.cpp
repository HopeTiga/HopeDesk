#include "WebrtcManager.h"

#include <future>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/field_trials.h>
#include <immintrin.h>
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

WebrtcManager::WebrtcManager()
    : ioContext(1)
    , accept(ioContext,boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"),19998))
    , tcpSocket(nullptr)
    , ioContextWorkPtr(nullptr)
    , webSocket(nullptr)
    , asioConcurrentQueue(ioContext.get_executor())
    , webrtcAsioConcurrentQueue(ioContext.get_executor())
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
}

void WebrtcManager::asyncEvent(){

    if(asyncAccpets.exchange(true)) return;

    boost::asio::co_spawn(ioContext,[self = shared_from_this()]()->boost::asio::awaitable<void>{

        while(self->asyncAccpets.load()){

            std::shared_ptr<boost::asio::ip::tcp::socket> socket = std::make_shared<boost::asio::ip::tcp::socket>(self->ioContext);

            try {
                co_await self->accept.async_accept(*socket,boost::asio::use_awaitable);
            } catch (...) {
                co_return;
            }

            self->closeTcpSocket();

            self->tcpSocket = std::move(socket);

            LOG_INFO("tcpSocket Accept Successful!");

            self->asyncEvents = true;

            self->followRunning = true;

            self->asioConcurrentQueue.reset();

            self->receiveCoroutineAysnc();

            boost::asio::co_spawn(self->ioContext,self->writerCoroutineAsync(),boost::asio::detached);

            boost::json::object registerJson;
            registerJson["requestType"] = 0;
            registerJson["systemService"] = self->webrtcManagerConfig.systemService;
            registerJson["stunHost"]    = self->webrtcManagerConfig.stunHost;
            registerJson["turnHost"]    = self->webrtcManagerConfig.turnHost;
            registerJson["turnUsername"] = self->webrtcManagerConfig.turnUsername;
            registerJson["turnPassword"] = self->webrtcManagerConfig.turnPassword;
            registerJson["debugLog"]    = self->webrtcManagerConfig.webrtcDebugLog;   // 调试开关随注册发给 System
            registerJson["state"]       = 200;
            std::string registerStr = boost::json::serialize(registerJson);

            std::shared_ptr<WriterData> registerData = std::make_shared<WriterData>(registerStr.data(), registerStr.size());

            self->asyncWrite(registerData);

            std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(self->followData.data(), self->followData.size());

            self->asyncWrite(writerData);

        }

    },boost::asio::detached);

}

void WebrtcManager::closeEvent(){

    if(!asyncAccpets.exchange(false)) return;

    accept.cancel();

    accept.close();

}

void WebrtcManager::connect(std::string ip)
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

        std::shared_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> ws;

        try {

            if(self->webSocket){

                self->closeWebSocket();

                self->webSocket = nullptr;

            }

            ws = std::make_shared<boost::beast::websocket::stream<
                boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>>(self->ioContext, self->sslContext);

            self->webSocket = ws;

            boost::asio::ip::tcp::resolver resolver(self->ioContext);

            auto results = co_await resolver.async_resolve(
                host, port,
                boost::asio::cancel_after(TIME_OUT, boost::asio::use_awaitable)
                );

            if (results.empty()) {
                throw std::runtime_error("resolve returned empty results (timeout or cancel)");
            }

            co_await boost::asio::async_connect(
                ws->next_layer().next_layer(),
                results,
                boost::asio::cancel_after(TIME_OUT, boost::asio::use_awaitable)
                );

            // 3. SSL 握手（带超时）
            co_await ws->next_layer().async_handshake(
                boost::asio::ssl::stream_base::client,
                boost::asio::cancel_after(TIME_OUT, boost::asio::use_awaitable)
                );

            ws->set_option(boost::beast::websocket::stream_base::decorator(
                [accountId = self->accountId](boost::beast::websocket::request_type& req) {
                    req.set(boost::beast::http::field::authorization, accountId);
                }));

            co_await ws->async_handshake(
                host, "/",
                boost::asio::cancel_after(TIME_OUT, boost::asio::use_awaitable)
                );

            self->webrtcAsioConcurrentQueue.reset();

            self->setTcpKeepAlive(ws->next_layer().next_layer());

            boost::asio::co_spawn(self->ioContext, self->webrtcReceiveCoroutine(), boost::asio::detached);

            boost::asio::co_spawn(self->ioContext, self->webrtcWriteCoroutine(), boost::asio::detached);

            if (self->onSignalServerConnectHandle) {

                self->onSignalServerConnectHandle();

            }

        }
        catch (std::exception & e) {

            // 取消/eof/断开属拆除或重连取消在途 connect 的预期情况,降 WARN;
            // 其余(服务器不可达、握手失败等)仍记 ERROR。
            bool aborted = false;
            if (auto se = dynamic_cast<const boost::system::system_error*>(&e)) {
                auto ec = se->code();
                aborted = ec == boost::asio::error::operation_aborted ||
                          ec == boost::asio::error::eof ||
                          ec == boost::asio::error::connection_aborted ||
                          ec == boost::asio::error::connection_reset;
            }
            if (aborted) LOG_WARN("WebSocket Connect aborted: %s", e.what());
            else LOG_ERROR("WebSocket Connect Error : %s",e.what());

            if (self->webSocket == ws && ws) {
                self->closeWebSocket();
            }

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

WebrtcManager::~WebrtcManager()
{
    LOG_INFO("Destructing WebrtcManager...");

    std::promise<void> promise;

    std::future<void> future = promise.get_future();

    closeEvent();

    boost::asio::post(ioContext,[this,&promise](){

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

        promise.set_value();

    });

    future.get();

    if (ioContextWorkPtr) {
        ioContextWorkPtr.reset();
    }

    ioContext.stop();
    if(ioContextThread.joinable()){

        ioContextThread.join();

    }

    LOG_INFO("WebrtcManager Destructed.");
}

bool WebrtcManager::initializePeerConnection()
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

        std::unique_ptr<WebrtcVideoEncoderFactory> webrtcVideoEncoderFactoryUnique = std::make_unique<WebrtcVideoEncoderFactory>();

        webrtcVideoEncoderFactory = webrtcVideoEncoderFactoryUnique.get();

        std::unique_ptr<WebrtcVideoDecoderFactory> webrtcVideoDecoderFactoryUnique = std::make_unique<WebrtcVideoDecoderFactory>();

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

    config.ice_connection_receiving_timeout = 10000;        // 连上后 10s 收不到数据才判断开(检测断开保持 10s)

    config.ice_unwritable_timeout = 15000;                  // 建立阶段 15s 无写成功才标记不可写

    config.ice_inactive_timeout = 15000;                    // 15s 无活动才标记非活跃

    config.set_dscp(true);

    webrtc::PeerConnectionInterface::IceServer stunServer;

    stunServer.uri = webrtcManagerConfig.stunHost;

    config.servers.push_back(stunServer);

    webrtc::PeerConnectionInterface::IceServer turnServer;

    turnServer.uri = webrtcManagerConfig.turnHost;

    turnServer.username = webrtcManagerConfig.turnUsername;

    turnServer.password = webrtcManagerConfig.turnPassword;

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

void WebrtcManager::asyncWrite(std::shared_ptr<WriterData> writerData){
    asioConcurrentQueue.enqueue(std::move(writerData));
}

void WebrtcManager::webrtcAsyncWrite(std::string str)
{
    webrtcAsioConcurrentQueue.enqueue(std::move(str));
}

void WebrtcManager::post(std::function<void()> task)
{
    boost::asio::post(ioContext, std::move(task));
}

void WebrtcManager::disConnectRemote()
{
    if(onResetCursorHandle) onResetCursorHandle();

    // 统一 post 到 ioContext 执行:线程安全,且无条件清理(不再因 isRemote==false 早退而漏清 tcpSocket)
    bool wasRemote = isRemote.exchange(false);
    boost::asio::post(ioContext, [self = shared_from_this(), wasRemote]() {
        self->closeTcpSocket();
        self->releaseSource();            // 含 closeTcpSocket(此时已 null)+ 停服务
        self->initializePeerConnection();
        if (wasRemote && self->webSocket && self->webSocket->is_open()) {
            boost::json::object message;
            message["accountId"] = self->accountId;
            message["targetId"] = self->targetId;
            message["requestType"] = static_cast<int64_t>(WebrtcRequestState::STOPREMOTE);
            self->webrtcAsyncWrite(boost::json::serialize(message));
        }
    });
}

void WebrtcManager::requestStats()
{
    if (!peerConnection) return;

    // 每次新建一个回调对象,避免复用同一对象带来的状态/时序问题
    auto handle = webrtc::make_ref_counted<hope::rtc::RTCStatsCollectorHandle>();
    if (onRTCStatsCollectorHandle) {
        handle->onRTCStatsCollectorHandle = onRTCStatsCollectorHandle;
    }
    peerConnection->GetStats(handle.get());
}

void WebrtcManager::disConnectRemoteHandler()
{
    if(onResetCursorHandle) onResetCursorHandle();

    // 统一 post 到 ioContext 执行:线程安全,且无条件清理(不再因 isRemote==false 早退而漏清 tcpSocket)。
    // 区分两种断开:已连上(曾 isRemote)后断开 -> onDisConnectRemoteHandle;
    // 从未连上(连接失败/被控端 System 未起来) -> onRemoteFailedHandle。
    bool wasRemote = isRemote.exchange(false);
    boost::asio::post(ioContext, [self = shared_from_this(), wasRemote]() {
        self->closeTcpSocket();
        self->releaseSource();
        self->initializePeerConnection();
        if (wasRemote) {
            if (self->onDisConnectRemoteHandle) self->onDisConnectRemoteHandle();
        } else {
            if (self->onRemoteFailedHandle) self->onRemoteFailedHandle();
        }
    });
}

void WebrtcManager::closeWebSocket()
{

    webrtcAsyncEvents.store(false);

    boost::system::error_code ec;

    webrtcAsioConcurrentQueue.close();

    if(!webSocket) return;

    boost::asio::ip::tcp::socket & tcpSocket = webSocket->next_layer().next_layer();

    tcpSocket.cancel(ec);
    if (ec) {
        LOG_WARN("WebrtcManager::closeSocket() can't cancel Socket: %s", ec.message().c_str());
    }

    if (webSocket->is_open()) {
        try {
            webSocket->close(boost::beast::websocket::close_code::normal, ec);
        }
        catch (const std::exception& e) {
            LOG_ERROR("WebrtcManager::closeSocket() close WebSocket failed: %s", e.what());
        }
    }

    // SSL 关闭
    if (webSocket->next_layer().next_layer().is_open()) {
        webSocket->next_layer().shutdown(ec);
        webSocket->next_layer().next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        webSocket->next_layer().next_layer().close(ec);
        if (ec && ec != boost::asio::error::not_connected) {
            LOG_ERROR("WebrtcManager::closeSocket() close Tcp Socket failed: %s", ec.message().c_str());
        }
    }

    webSocket = nullptr;

    LOG_INFO("WebrtcManager::WebSocket is close");
}

void WebrtcManager::setTcpKeepAlive(boost::asio::ip::tcp::socket &sock, int idle, int intvl, int probes)
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

boost::asio::awaitable<void> WebrtcManager::webrtcReceiveCoroutine()
{
    webrtcAsyncEvents.store(true);

    // 持有当前 webSocket 的共享引用，避免挂起期间成员被重连/关闭置空后对象被销毁
    std::shared_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> ws = webSocket;

    if (!ws) co_return;

    try{

        while (webrtcAsyncEvents.load()) {

            boost::beast::flat_buffer buffer;

            co_await ws->async_read(buffer, boost::asio::use_awaitable);

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

            std::string dataStr = boost::json::serialize(json);

            if(this->tcpSocket && this->tcpSocket->is_open() && WebrtcRequestState(json["requestType"].as_int64()) == WebrtcRequestState::REQUEST){

                std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(),dataStr.size());

                asyncWrite(writerData);

                continue;
            }

            if(json.contains("requestType")){

                int64_t requestType = json["requestType"].as_int64();

                int64_t responseState = json["state"].as_int64();

                if(WebrtcRequestState(requestType) == WebrtcRequestState::REQUEST){

                    if(responseState == 200){

                        if(json.contains("type")){
                            std::string type(json["type"].as_string().c_str());

                            if(type == "request"){

                                targetId = std::string(json["accountId"].as_string().c_str());

                                json["localMaxBitrateBps"] = webrtcDeskConfig.localMaxBitrateBps;

                                json["localMinBitrateBps"] = webrtcDeskConfig.localMinBitrateBps;

                                json["localMaxFramerate"] = webrtcDeskConfig.localMaxFramerate;

                                json["desktopWidth"] = webrtcDeskConfig.desktopWidth;

                                json["desktopHeight"] = webrtcDeskConfig.desktopHeight;

                                json["desktopRefreshRate"] = webrtcDeskConfig.desktopRefreshRate;

                                this->followData = boost::json::serialize(json);

                                WindowsServiceManager::stopService(webrtcManagerConfig.systemService);

                                if(!WindowsServiceManager::serviceExists(webrtcManagerConfig.systemService)){

                                    WindowsServiceManager::registerService(webrtcManagerConfig.systemService, webrtcManagerConfig.systemServiceExe);

                                }

                                if (WindowsServiceManager::startService(webrtcManagerConfig.systemService)) {

                                    LOG_INFO("WindowsServiceManager::startService Successful!");

                                    // 唯一看门狗:多次 request 只保留一个 timer,超时刷新不会失效
                                    this->armRequestTimeout();

                                    continue;

                                }
                            }else if(type == "offer"){

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

                }else if(WebrtcRequestState(requestType) == WebrtcRequestState::STOPREMOTE){

                    if(responseState == 200){

                        disConnectRemoteHandler();

                    }

                }else if(WebrtcRequestState(requestType) == WebrtcRequestState::CLOSESYSTEM){

                    if(responseState == 200){

                        closeTcpSocket();

                        WindowsServiceManager::stopService(webrtcManagerConfig.systemService);

                        targetId = json["accountId"].as_string().c_str();

                        boost::json::object request;

                        request["requestType"] = static_cast<int>(WebrtcRequestState::SYSTEMREADLY);

                        request["accountId"] = accountId;

                        request["targetId"] = targetId;

                        webrtcAsyncWrite(boost::json::serialize(request));

                    }

                }else if(WebrtcRequestState(requestType) == WebrtcRequestState::SYSTEMREADLY){

                    if(responseState == 200){

                        asyncRemoteDesk(webrtcDeskConfig);

                    }

                }
            }


        }

    }catch(std::exception & e){

        LOG_ERROR("WebSocket Connect Error : %s",e.what());

        if (webSocket == ws) {
            closeWebSocket();
        }

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

boost::asio::awaitable<void> WebrtcManager::webrtcWriteCoroutine()
{
    std::shared_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>> ws = webSocket;

    if (!ws) co_return;

    try {

        while (webrtcAsyncEvents.load()) {

            std::optional<std::string> optional = co_await webrtcAsioConcurrentQueue.dequeue();

            if (optional.has_value()) {

                std::string str = std::move(optional.value());

                co_await ws->async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

            }else break;

            if (!webrtcAsyncEvents.load()) break;

        }

    }
    catch (const std::exception& e) {

        LOG_ERROR("Writer coroutine unhandled exception: %s", e.what());

        if (webSocket == ws) {
            closeWebSocket();
        }

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

    }
    catch (...) {

        LOG_ERROR("Writer coroutine unknown exception");

        if (webSocket == ws) {
            closeWebSocket();
        }

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

    }
    co_return;
}

void WebrtcManager::disConnectHandle()
{

    if(onResetCursorHandle) onResetCursorHandle();

    closeTcpSocket();

    releaseSource();

    initializePeerConnection();

}

void WebrtcManager::setWebrtcManagerConfig(const WebrtcManagerConfig& webrtcManagerConfig)
{
    this->webrtcManagerConfig = webrtcManagerConfig;
}

void WebrtcManager::applyWebrtcDebugLog(bool enabled)
{
    // 运行期改 debugLog:整个操作 post 到 ioContext,避免 GUI 线程与 ioContext 竞争 webrtcManagerConfig/tcpSocket。
    // 若本地 TCP(System)已连则重发 REGISTER(含 debugLog);REGISTER 幂等,重发安全。
    post([self = shared_from_this(), enabled]() {
        self->webrtcManagerConfig.webrtcDebugLog = enabled;
        if (!self->tcpSocket || !self->tcpSocket->is_open()) return;

        boost::json::object registerJson;
        registerJson["requestType"] = 0;
        registerJson["systemService"] = self->webrtcManagerConfig.systemService;
        registerJson["stunHost"]    = self->webrtcManagerConfig.stunHost;
        registerJson["turnHost"]    = self->webrtcManagerConfig.turnHost;
        registerJson["turnUsername"] = self->webrtcManagerConfig.turnUsername;
        registerJson["turnPassword"] = self->webrtcManagerConfig.turnPassword;
        registerJson["debugLog"]    = self->webrtcManagerConfig.webrtcDebugLog;
        registerJson["state"]       = 200;

        std::string registerStr = boost::json::serialize(registerJson);
        auto registerData = std::make_shared<WriterData>(registerStr.data(), registerStr.size());
        self->asyncWrite(registerData);
        LOG_INFO("applyWebrtcDebugLog: pushed debugLog=%d to System", enabled ? 1 : 0);
    });
}

void WebrtcManager::resetCursorCache()
{
    cursorArray.clear();
    cursorCacheDirty = true;  // handleCursor 首次执行时据此重置 lastCursor
    cursorResyncRequested = false;  // 新连接:重置节流,允许按需重同步
}

void WebrtcManager::requestCursorResync()
{
    // 节流:同一轮"无效索引连发"只发一次重同步请求,直到对端重新全量发回数据
    if (cursorResyncRequested.exchange(true)) return;

    // 清空本地索引缓存,与对端一起回到 index 从 0 开始的干净状态
    cursorArray.clear();
    LOG_WARN("Cursor index invalid -> request peer resync (type=7), local cache cleared");

    if (!dataChannel) return;

    // type=7:请求对端清空全部光标索引缓存,并从 index 0 重新全量发送(type=1 数据 + type=0 引用)
#pragma pack(push, 1)
    struct CursorResyncReq {
        short type;   // 7
    };
#pragma pack(pop)
    CursorResyncReq* pkt = new CursorResyncReq{7};
    writerRemote(reinterpret_cast<unsigned char*>(pkt), sizeof(CursorResyncReq));
}

void WebrtcManager::handleCursor(const unsigned char *data, size_t size)
{
    // Use thread-local storage to avoid thread safety issues
    static thread_local HCURSOR lastCursor = nullptr;

    // 重连清缓存:新会话首次执行时重置 lastCursor,使光标样式从 type=1 重新全量同步,
    // 避免与对端(每连接 index 从 0)错位产生 Invalid cursor index。
    if (cursorCacheDirty.exchange(false)) {
        lastCursor = nullptr;
    }

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
    case 0: { // Cursor index message (隐式：光标可见 -> 绝对模式)
        relativeMouseMode = false;
        if (size < sizeof(CursorMessage)) {
            LOG_ERROR("Invalid cursor index message size");
            break;
        }

        const CursorMessage* msg = reinterpret_cast<const CursorMessage*>(data);

        // CRITICAL: Validate index bounds
        if (msg->index < 0 || msg->index >= this->cursorArray.size()) {
            LOG_ERROR("Invalid cursor index: %d (array size: %zu)", msg->index, this->cursorArray.size());
            // 本地缓存与对端索引错位:请求双方清空、从 0 重新全量同步
            requestCursorResync();
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
            cursorResyncRequested = false;  // 已成功应用,重同步完成,允许下次再请求
        }
        break;
    }

    case 1: { // New cursor data (隐式：光标可见 -> 绝对模式)
        relativeMouseMode = false;
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
            // 索引跳号/乱序:本地无法按 vector 顺序存储,请求双方清空、从 0 重新全量同步
            requestCursorResync();
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
            cursorResyncRequested = false;  // 已成功存储并应用,重同步完成
        }
        break;
    }

    case 2: { // 隐藏光标 -> 进入相对鼠标模式(游戏视角)
        relativeMouseMode = true;

        // 用 1x1 全透明光标替换系统普通光标，实现本地隐藏
        static HCURSOR transparentCursor = nullptr;
        if (!transparentCursor) {
            unsigned char zero[4] = { 0, 0, 0, 0 }; // RGBA 全 0，alpha=0 -> 全透明
            transparentCursor = CreateCursorFromRGBA(zero, 1, 1, 0, 0);
        }
        if (transparentCursor) {
            // SetSystemCursor 会销毁传入的句柄，需每次拷贝一份
            HCURSOR copy = CopyCursor(transparentCursor);
            if (copy) {
                SetSystemCursor(copy, 32512); // OCR_NORMAL
            }
        }
        break;
    }

    default:
        LOG_WARN("Unknown message type: %d", type);
        break;
    }
}

boost::asio::awaitable<void> WebrtcManager::writerCoroutineAsync()
{
    try {

        std::shared_ptr<boost::asio::ip::tcp::socket> socket = this->tcpSocket;

        while(asyncEvents.load()){

            std::optional<std::shared_ptr<WriterData>> optional = co_await asioConcurrentQueue.dequeue();

            if(optional.has_value()){

                std::shared_ptr<WriterData> writeData = optional.value();

                co_await boost::asio::async_write(*socket,boost::asio::buffer(writeData->data,writeData->size),boost::asio::use_awaitable);

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

void WebrtcManager::receiveCoroutineAysnc()
{
    boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {

        std::shared_ptr<boost::asio::ip::tcp::socket> socket = this->tcpSocket;

        char headerBuffer[8];
        size_t headerSize = sizeof(int64_t);
        int messageCount = 0;

        while (asyncEvents) {
            std::memset(headerBuffer, 0, headerSize);

            // 接收消息头
            size_t headerRead = 0;
            while (headerRead < headerSize) {
                size_t n = co_await socket->async_read_some(
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
                size_t n = co_await socket->async_read_some(
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

            if(WebrtcRequestState(json["requestType"].as_int64()) == WebrtcRequestState::START){

                this->onFollowRemoteHandle();

                this->isRemote = true;

                continue;

            }else if(WebrtcRequestState(json["requestType"].as_int64()) == WebrtcRequestState::STATS){

                int type = json["connectionType"].as_int64();

                if(onRTCStatsCollectorHandle){

                    onRTCStatsCollectorHandle(type, -1.0);

                }

            }else if(WebrtcRequestState(json["requestType"].as_int64()) == WebrtcRequestState::ENCODE_STATUS){

                // 被控端:本机 System 经本地 TCP 上报当前编码 codec + 硬编/软编 + 采集技术
                std::string codec = json.contains("codec") ? json["codec"].as_string().c_str() : "";
                bool hard = json.contains("hard") ? (json["hard"].as_bool()) : false;
                std::string capture = json.contains("capture") ? json["capture"].as_string().c_str() : "Hope Virtual Display";
                LOG_INFO("Encode status from System: codec=%s hard=%d capture=%s", codec.c_str(), hard ? 1 : 0, capture.c_str());
                if (onEncodeStatusHandle) onEncodeStatusHandle(codec, hard, capture);

                continue;  // 状态消息不再转发给信号服务器
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

                                disConnectRemoteHandler();

                                LOG_WARN("Reader coroutine handler exception: %s", e.what());

                              }
                          });
}

void WebrtcManager::sendSignalingMessage(boost::json::object& msg) {

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

    msg["requestType"] = static_cast<int64_t>(WebrtcRequestState::REQUEST);
    msg["state"] = 200;

    try {
        webrtcAsyncWrite(boost::json::serialize(msg));
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to send signaling message: %s",e.what());
    }
}

void WebrtcManager::closeTcpSocket()
{
    // 统一的 tcpSocket 清理:停收发队列、关 socket、置空。需在 ioContext 线程调用。
    if (!tcpSocket) return;
    asyncEvents = false;
    followRunning = false;
    asioConcurrentQueue.close();
    if (tcpSocket->is_open()) {
        tcpSocket->close();
    }
    tcpSocket = nullptr;
}

void WebrtcManager::releaseSource()
{

    // 断开时清空工厂缓存的渲染 D3D11 设备:下个连接用新窗口/新设备,
    // 避免新解码器先拿到上一连接(可能已销毁)的旧设备初始化。
    if (webrtcVideoDecoderFactory) {
        webrtcVideoDecoderFactory->clearDecoderD3D11Device();
    }

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

    closeTcpSocket();

    WindowsServiceManager::stopService(webrtcManagerConfig.systemService);  // ← 也可能在这里阻塞

}

std::string WebrtcManager::getAccountId() const
{
    return accountId;
}

void WebrtcManager::setAccountId(const std::string &newAccountId)
{
    accountId = newAccountId;
}

void WebrtcManager::setDecoderD3D11Device(ID3D11Device* dev)
{
    if (webrtcVideoDecoderFactory) {
        webrtcVideoDecoderFactory->setDecoderD3D11Device(dev);
    }
}

void WebrtcManager::armRequestTimeout()
{
    // 只在 ioContext 线程调用(asyncRemoteDesk / 收包协程均在 ioContext 上),无数据竞争。
    // 唯一看门狗:30 秒内再发请求会 cancel 旧 timer 并重建,旧协程按 operation_aborted/纪元不匹配退出。
    // 取 30s 与 ICE 容错(unwritable/inactive=30s)一致:握手慢时让 ICE 自己兜底,看门狗不抢先拆连接。
    uint64_t epoch = ++timeoutEpoch;

    if (requestTimeout) {
        requestTimeout->cancel();  // 旧 timer 挂起的 async_wait 以 operation_aborted 返回
    }

    requestTimeout = std::make_shared<boost::asio::steady_timer>(ioContext);
    requestTimeout->expires_after(std::chrono::seconds(15));

    auto timer = requestTimeout;

    boost::asio::co_spawn(ioContext,[self = shared_from_this(), timer, epoch]()mutable->boost::asio::awaitable<void>{

        boost::system::error_code ec;

        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) co_return;  // 被新一轮请求刷新取消

        if (epoch != self->timeoutEpoch) co_return;                   // 兜底:纪元对不上说明已过期

        if (!self->isRemote) {

            self->closeTcpSocket();

            self->releaseSource();

            self->initializePeerConnection();

            if (self->onRemoteFailedHandle) self->onRemoteFailedHandle();

            LOG_INFO("WebRTCManager Request Timeout ReInit");

        }

    },boost::asio::detached);
}

void WebrtcManager::asyncRemoteDesk(WebrtcDeskConfig webrtcDeskConfig)
{

    this->webrtcDeskConfig = webrtcDeskConfig;

    boost::asio::co_spawn(ioContext,[self = shared_from_this()]()->boost::asio::awaitable<void>{

        if(self->peerConnection == nullptr){

            self->initializePeerConnection();

        }

        if (self->webrtcVideoDecoderFactory) {
            self->webrtcVideoDecoderFactory->webrtcEnableD3D11 = self->webrtcDeskConfig.webrtcEnableD3D11;
            // 解码状态 -> 转发给 onCodecStatusHandle(MainWindow 据此更新主页 label)
            self->webrtcVideoDecoderFactory->onDecoderStatusHandle =
                [self](const std::string& codec, bool hardDecode) {
                    if (self->onCodecStatusHandle) self->onCodecStatusHandle(codec, hardDecode);
                };
            // 硬解直投:帧回调同步给工厂(下发给存活/新建的硬解解码器)。
            self->webrtcVideoDecoderFactory->setOnDisplayHandle(self->onVideoFrameHandler);
            LOG_INFO("asyncRemoteDesk: set decoder factory webrtcEnableD3D11=%d", self->webrtcDeskConfig.webrtcEnableD3D11);
        } else {
            LOG_WARN("asyncRemoteDesk: webrtcVideoDecoderFactory is null, hard decode disabled");
        }

        if (self->targetId.empty()) {
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
            message["requestType"] = static_cast<int64_t>(WebrtcRequestState::REQUEST);
            message["type"] = "request";
            message["webrtcModulesType"] = self->webrtcDeskConfig.webrtcModulesType;
            message["webrtcUseLevels"] = self->webrtcDeskConfig.webrtcUseLevels;
            message["codec"] = self->webrtcDeskConfig.videoCodec;
            message["webrtcAudioEnable"] = self->webrtcDeskConfig.webrtcAudioEnable;
            message["webrtcEnableNvenc"] = self->webrtcDeskConfig.webrtcEnableNvenc;
            // 编码配置-请求组:发给远端 System(本地组不在此发送)
            message["requestMaxBitrateBps"] = self->webrtcDeskConfig.requestMaxBitrateBps;
            message["requestMinBitrateBps"] = self->webrtcDeskConfig.requestMinBitrateBps;
            message["requestMaxFramerate"] = self->webrtcDeskConfig.requestMaxFramerate;

            self->webrtcAsyncWrite(boost::json::serialize(message));

            LOG_INFO("Request sent to target: %s", self->targetId.c_str());

            // 唯一看门狗:多次请求只保留一个 timer,最后一次请求发出 15s 未连上才触发一次 re-init
            self->armRequestTimeout();
        }

    },boost::asio::detached);

}

std::string WebrtcManager::getTargetId() const
{
    return targetId;
}

void WebrtcManager::setTargetId(const std::string &newTargetId)
{
    targetId = newTargetId;
}

void WebrtcManager::setWebrtcDeskConfig(const WebrtcDeskConfig &config)
{
    webrtcDeskConfig = config;
}

void WebrtcManager::abortPendingConnection()
{
    // 统一入口:post 到 ioContext 执行,线程安全。
    // 无条件重置连接态(不论 isRemote),清掉 tcpSocket/peerConnection 并重建空白 peerConnection,
    // 保留 webSocket(信令连接不断,便于立即重试)。解决超时/ICE failed 后 tcpSocket 残留导致重连失败。
    boost::asio::post(ioContext, [self = shared_from_this()]() {
        self->isRemote = false;
        self->releaseSource();            // 关 peerConnection/dataChannel/tcpSocket + 停服务
        self->initializePeerConnection();
        LOG_INFO("abortPendingConnection: connection state reset");
    });
}

void WebrtcManager::sendKeyComboCtrlAltF()
{
    if (!dataChannel) {
        LOG_ERROR("sendKeyComboCtrlAltF: dataChannel null");
        return;
    }
#pragma pack(push,1)
    struct KeyButton { short type; DWORD buttonId; char modifiers; };
#pragma pack(pop)
    // 对端 KeyDown/KeyUp 逐键发扫描码(modifiers 字段忽略),所以按顺序发
    // Ctrl↓ Alt↓ F↓ F↑ Alt↑ Ctrl↑ 即可让对端 OS 收到 Ctrl+Alt+F 组合。
    struct Step { short type; DWORD vk; };
    const Step steps[] = {
        {3, VK_CONTROL}, {3, VK_MENU}, {3, static_cast<DWORD>('F')},
        {4, static_cast<DWORD>('F')}, {4, VK_MENU}, {4, VK_CONTROL},
    };
    for (const auto& s : steps) {
        KeyButton* pkt = new KeyButton{s.type, s.vk, 0};
        writerRemote(reinterpret_cast<unsigned char*>(pkt), sizeof(KeyButton));
    }
    LOG_INFO("sendKeyComboCtrlAltF sent");
}

void WebrtcManager::writerRemote(unsigned char *data, size_t size)
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

void WebrtcManager::setOnVideoFrameHanlder(std::function<void(std::shared_ptr<VideoFrame>)> onVideoFrameHandler)
{
    this->onVideoFrameHandler = onVideoFrameHandler;
    // 硬解直投:同一回调也交给解码器工厂;软解仍走 sink。
    if (webrtcVideoDecoderFactory) {
        webrtcVideoDecoderFactory->setOnDisplayHandle(onVideoFrameHandler);
    }
}

void WebrtcManager::disConnect()
{

    boost::asio::post(ioContext,[self = shared_from_this()](){

        // 注意:不在此调 closeEvent()。acceptor 只应在进程关停(析构)时停,
        // 正常断开要保留 acceptor 以便再次连接;且 closeEvent 在此调会让 acceptor 协程
        // 在 ioContext 线程释放最后强引用,导致析构跑在 ioContext 线程 → post+future.get 死锁。

        if (self->webSocket && self->webSocket->is_open()) {

            self->closeWebSocket();

            self->webSocket = nullptr;
        }

        self->closeTcpSocket();

        if(self->onDisConnectRemoteHandle){

            self->onDisConnectRemoteHandle();

        }

        self->disConnectHandle();

    });

}


}

}
