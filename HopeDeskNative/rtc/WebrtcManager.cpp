#include "WebrtcManager.h"

#include <future>

#include <ylt/struct_pack.hpp>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/field_trials.h>
#include <immintrin.h>
#include "../utils/Utils.h"

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
    , ioContextWorkPtr(nullptr)
    , peerConnection(nullptr)
{

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this](){
        this->ioContext.run();
    }));
}

void WebrtcManager::asyncEvent(){

    if (tcpAcceptor) return;

    tcpAcceptor = std::make_shared<TcpAcceptor>(ioContext, 19998);

    tcpAcceptor->setOnAcceptHandle([this](std::shared_ptr<TcpSocket> connection) {
        connection->setOnMessageHandle([this](std::string str) {
            handleSystemMessage(std::move(str));
        });
        connection->setOnDisConnectHandle([this]() {
            handleSystemDisconnect();
        });
        tcpSocket = connection;
        handleSystemAccept();
    });

    tcpAcceptor->startAccept();

    LOG_INFO("WebrtcManager local TCP accept started on 127.0.0.1:19998");
}

void WebrtcManager::closeEvent(){

    if (tcpAcceptor) {
        tcpAcceptor->stopAccept();
    }

}

void WebrtcManager::connect(std::string ip)
{
    std::string host = ip;
    std::string port = "443"; // 默认端口，你可以根据需要修改

    size_t colonPos = ip.find(':');
    if (colonPos != std::string::npos) {
        host = ip.substr(0, colonPos);
        port = ip.substr(colonPos + 1);
    }

    boost::asio::co_spawn(ioContext, [self = shared_from_this(), host, port, this]()mutable->boost::asio::awaitable<void> {

        if (self->webSocket) {
            self->webSocket->closeEvent();
            self->webSocket.reset();
        }

        std::shared_ptr<WebSocket> ws = std::make_shared<WebSocket>(self->ioContext);

        self->webSocket = ws;

        ws->setOnMessageHandle([this](std::string str) {
            handleSignalMessage(std::move(str));
        });

        ws->setOnConnectHandle([this]() {
            if (onSignalServerConnectHandle) {
                onSignalServerConnectHandle();
            }
        });

        ws->setOnDisConnectHandle([this]() {
            handleSignalServerDisconnect();
        });

        utils::Options httpHeaders;

        httpHeaders["authorization"] = self->accountId;

        bool connected = co_await ws->connect(host, port, "/", httpHeaders);

        if (!connected && self->webSocket == ws) {

            self->webSocket.reset();
        }

        co_return;
    }, boost::asio::detached);


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

        if (webSocket) {
            webSocket->closeEvent();
            webSocket.reset();
        }

        releaseSource();

        if (tcpAcceptor) {
            tcpAcceptor.reset();
        }

        if (tcpSocket) {
            tcpSocket.reset();
        }

        webrtcVideoDecoderFactory = nullptr;

        webrtcVideoEncoderFactory = nullptr;

        peerConnectionFactory = nullptr;

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
            webrtcVideoEncoderFactory = nullptr;
            webrtcVideoDecoderFactory = nullptr;
            return false;
        }
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

    config.ice_connection_receiving_timeout = 10000;        // 连上后 10s 收不到数据才判断开(检测断开保持 10s)

    config.ice_unwritable_timeout = 30000;                  // 建立阶段 30s 无写成功才标记不可写

    config.ice_inactive_timeout = 30000;                    // 30s 无活动才标记非活跃

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
    if (tcpSocket) {
        tcpSocket->asyncWrite(std::move(writerData));
    }
}

bool WebrtcManager::webrtcAsyncWrite(std::string str)
{

    if (!webSocket) {
        LOG_ERROR("webrtcAsyncWrite: websocket is null, message dropped");
        return false;
    }
    if (!webSocket->asyncWrite(std::move(str))) {
        LOG_ERROR("webrtcAsyncWrite: websocket send queue is closed, message dropped");
        return false;
    }
    return true;
}

void WebrtcManager::post(std::function<void()> task)
{
    boost::asio::post(ioContext, std::move(task));
}

void WebrtcManager::disConnectRemote()
{
    if(onResetCursorHandle) onResetCursorHandle();

    boost::asio::post(ioContext, [self = shared_from_this()]() {
        self->cancelRequestTimeout();     // 主动断开:取消挂起的看门狗,防止连接结束后幽灵 teardown
        self->closeTcpSocket();
        self->releaseSource();            // 含 closeTcpSocket(此时已 null)+ 停服务
        self->initializePeerConnection();
        if (self->webSocket && self->webSocket->isOpen()) {
            WebrtcEnvelope webrtcEnvelope;
            webrtcEnvelope.requestType = static_cast<int>(WebrtcRequestState::STOPREMOTE);
            webrtcEnvelope.state = 200;   // 接收端按 state==200 处理;body 为空:STOPREMOTE 无业务载荷
            webrtcEnvelope.accountId = self->accountId;
            webrtcEnvelope.targetId = self->targetId;
            self->webrtcAsyncWrite(struct_pack::serialize<std::string>(webrtcEnvelope));
        }
    });
}

void WebrtcManager::requestStats()
{
    if (!peerConnection) return;

    auto handle = webrtc::make_ref_counted<hope::rtc::RTCStatsCollectorHandle>();
    if (onRTCStatsCollectorHandle) {
        handle->onRTCStatsCollectorHandle = onRTCStatsCollectorHandle;
    }
    peerConnection->GetStats(handle.get());
}

void WebrtcManager::disConnectRemoteHandler()
{
    if(onResetCursorHandle) onResetCursorHandle();

    boost::asio::post(ioContext, [self = shared_from_this()]() {
        self->cancelRequestTimeout();
        self->closeTcpSocket();
        self->releaseSource();
        self->initializePeerConnection();
        if (self->isRemote.load()) {
            if (self->onDisConnectRemoteHandle) self->onDisConnectRemoteHandle();
        } else {
            if (self->onRemoteFailedHandle) self->onRemoteFailedHandle();
        }
    });
}

void WebrtcManager::handleSignalMessage(std::string str)
{

    WebrtcEnvelope webrtcEnvelope;

    size_t envelopeSize = 0;

    struct_pack::err_code deserializeError = struct_pack::deserialize_to(webrtcEnvelope, str, envelopeSize);
    if (deserializeError) {
        LOG_ERROR("WebSocket received invalid struct_pack: %s", deserializeError.message().data());
        return;
    }

    // 信封(WebrtcEnvelope)反序列化出 requestType/state/accountId/targetId,业务载荷是头部之后原样拼接的 body。
    // 用 deserialize_to 消耗的字节数切出 body,再把信封字段合并回 payload,还原成整包 JSON,供本地 System(仍讲 JSON)与下方逻辑使用。
    std::string payload = str.substr(envelopeSize);

    boost::json::object json;

    if (!payload.empty()) {
        try {
            json = boost::json::parse(payload).as_object();
        }
        catch (const std::exception& e) {
            LOG_ERROR("WebSocket received invalid payload JSON: %s", e.what());
            return;
        }
    }

    json["requestType"] = webrtcEnvelope.requestType;

    json["state"] = webrtcEnvelope.state;

    json["message"] = webrtcEnvelope.message;

    json["accountId"] = webrtcEnvelope.accountId;

    json["targetId"] = webrtcEnvelope.targetId;

    std::string dataStr = boost::json::serialize(json);

    if (json.contains("requestType") && tcpSocket && tcpSocket->isOpen() && WebrtcRequestState(json["requestType"].as_int64()) == WebrtcRequestState::REQUEST) {

        std::shared_ptr<WriterData> writerData = std::make_shared<WriterData>(dataStr.data(), dataStr.size());

        asyncWrite(writerData);

        return;
    }

    if (json.contains("requestType")) {

        int64_t requestType = json["requestType"].as_int64();

        int64_t responseState = json["state"].as_int64();

        if (WebrtcRequestState(requestType) == WebrtcRequestState::REQUEST) {

            if (responseState == 200) {

                if (json.contains("type")) {

                    std::string type(json["type"].as_string().c_str());

                    if (type == "request") {

                        targetId = std::string(json["accountId"].as_string().c_str());

                        json["localMaxBitrateBps"] = webrtcDeskConfig.localMaxBitrateBps;

                        json["localMinBitrateBps"] = webrtcDeskConfig.localMinBitrateBps;

                        json["localMaxFramerate"] = webrtcDeskConfig.localMaxFramerate;

                        json["desktopWidth"] = webrtcDeskConfig.desktopWidth;

                        json["desktopHeight"] = webrtcDeskConfig.desktopHeight;

                        json["desktopRefreshRate"] = webrtcDeskConfig.desktopRefreshRate;

                        this->followData = boost::json::serialize(json);

                        WindowsServiceManager::stopService(webrtcManagerConfig.systemService);

                        if (!WindowsServiceManager::serviceExists(webrtcManagerConfig.systemService)) {

                            WindowsServiceManager::registerService(webrtcManagerConfig.systemService, webrtcManagerConfig.systemServiceExe);

                        }

                        if (WindowsServiceManager::startService(webrtcManagerConfig.systemService)) {

                            LOG_INFO("WindowsServiceManager::startService Successful!");

                            this->armRequestTimeout(WebrtcRole::Callee);

                            return;
                        }

                    } else if (type == "offer") {

                        if (json.contains("accountId")) {
                            targetId = std::string(json["accountId"].as_string().c_str());
                        }

                        std::string sdp(json["sdp"].as_string().c_str());

                        if (!peerConnection) {
                            LOG_ERROR("PeerConnection is null, cannot process offer");
                            return;
                        }

                        // 创建远程描述
                        webrtc::SdpParseError error;
                        std::unique_ptr<webrtc::SessionDescriptionInterface> remoteDesc(
                            webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error));

                        if (!remoteDesc) {
                            LOG_ERROR("Failed to parse offer SDP: %s", error.description.c_str());
                            return;
                        }

                        peerConnection->SetRemoteDescription(
                            SetRemoteDescriptionObserver::Create().get(),
                            remoteDesc.release()
                            );

                        // 创建并发送 answer
                        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;

                        createAnswerObserver = CreateAnswerObserverImpl::Create(this, peerConnection);

                        peerConnection->CreateAnswer(createAnswerObserver.get(), options);

                    } else if (type == "candidate") {

                        std::string candidateStr(json["candidate"].as_string().c_str());
                        std::string mid = json.contains("mid") ? std::string(json["mid"].as_string().c_str()) : "";
                        int mlineIndex = json.contains("mlineIndex") ? static_cast<int>(json["mlineIndex"].as_int64()) : 0;

                        if (peerConnection) {

                            webrtc::SdpParseError error;
                            std::unique_ptr<webrtc::IceCandidateInterface> candidate(
                                webrtc::CreateIceCandidate(mid, mlineIndex, candidateStr, &error));

                            if (!candidate) {
                                LOG_ERROR("Failed to parse ICE candidate: %s", error.description.c_str());
                                return;
                            }

                            bool success = peerConnection->AddIceCandidate(candidate.get());
                            if (!success) {
                                LOG_ERROR("Failed to add ICE candidate");
                            }

                        } else {
                            LOG_ERROR("PeerConnection is null, cannot add ICE candidate");
                        }
                    }
                }

            } else if (responseState == 404) {

                if (onRemoteFailedHandle) {

                    onRemoteFailedHandle();

                }

            }

        } else if (WebrtcRequestState(requestType) == WebrtcRequestState::STOPREMOTE) {

            if (responseState == 200) {

                disConnectRemoteHandler();

            }

        } else if (WebrtcRequestState(requestType) == WebrtcRequestState::CLOSESYSTEM) {

            if (responseState == 200) {

                closeTcpSocket();

                WindowsServiceManager::stopService(webrtcManagerConfig.systemService);

                targetId = json["accountId"].as_string().c_str();

                WebrtcEnvelope webrtcEnvelope;

                webrtcEnvelope.requestType = static_cast<int>(WebrtcRequestState::SYSTEMREADLY);

                webrtcEnvelope.state = 200;   // 接收端按 state==200 处理

                webrtcEnvelope.accountId = accountId;

                webrtcEnvelope.targetId = targetId;

                webrtcAsyncWrite(struct_pack::serialize<std::string>(webrtcEnvelope));

            }

        } else if (WebrtcRequestState(requestType) == WebrtcRequestState::SYSTEMREADLY) {

            if (responseState == 200) {

                asyncRemoteDesk(webrtcDeskConfig);

            }

        }

    }

}

void WebrtcManager::handleSignalServerDisconnect()
{

    cancelRequestTimeout();  // 信令断连:取消挂起的看门狗

    if (onSignalServerDisConnectHandle) {

        onSignalServerDisConnectHandle();

    }

    if (isRemote == false) {

        return;

    }

    isRemote = false;

    if (onDisConnectRemoteHandle) {

        onDisConnectRemoteHandle();

    }

    releaseSource();

    initializePeerConnection();
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

    post([self = shared_from_this(), enabled]() {
        self->webrtcManagerConfig.webrtcDebugLog = enabled;
        if (!self->tcpSocket || !self->tcpSocket->isOpen()) return;

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

    if (cursorResyncRequested.exchange(true)) return;
    cursorArray.clear();
    LOG_WARN("Cursor index invalid -> request peer resync (type=7), local cache cleared");

    if (!dataChannel) return;

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
    static thread_local HCURSOR lastCursor = nullptr;

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

void WebrtcManager::handleSystemMessage(std::string str)
{
    boost::json::object json;
    try {
        json = boost::json::parse(str).as_object();
    }
    catch (const std::exception& e) {
        LOG_ERROR("handleSystemMessage: invalid JSON from System: %s", e.what());
        handleSystemDisconnect();
        return;
    }

    if (!json.contains("requestType")) {
        LOG_WARN("handleSystemMessage: missing requestType, drop");
        return;
    }

    WebrtcRequestState requestState = WebrtcRequestState(json["requestType"].as_int64());

    if (requestState == WebrtcRequestState::START) {

        if (onFollowRemoteHandle) onFollowRemoteHandle();

        isRemote = true;

        return;
    }

    if (requestState == WebrtcRequestState::STATS) {

        int type = json["connectionType"].as_int64();

        if (onRTCStatsCollectorHandle) onRTCStatsCollectorHandle(type, -1.0);

        return;
    }

    if (requestState == WebrtcRequestState::ENCODE_STATUS) {
        std::string codec = json.contains("codec") ? json["codec"].as_string().c_str() : "";
        bool hard = json.contains("hard") ? json["hard"].as_bool() : false;
        std::string capture = json.contains("capture") ? json["capture"].as_string().c_str() : "Hope Virtual Display";
        LOG_INFO("Encode status from System: codec=%s hard=%d capture=%s", codec.c_str(), hard ? 1 : 0, capture.c_str());
        if (onEncodeStatusHandle) onEncodeStatusHandle(codec, hard, capture);

        return;  // 状态消息不再转发给信号服务器
    }

    if (webSocket && webSocket->isOpen()) {
        // System 消息拆成信封 + 业务载荷:requestType/accountId/targetId 入信封,其余进 body
        WebrtcEnvelope webrtcEnvelope;
        webrtcEnvelope.requestType = static_cast<int>(json["requestType"].as_int64());
        webrtcEnvelope.state = 200;   // 接收端按 state==200 处理
        if (json.contains("accountId")) {
            webrtcEnvelope.accountId = std::string(json["accountId"].as_string().c_str());
        }
        if (json.contains("targetId")) {
            webrtcEnvelope.targetId = std::string(json["targetId"].as_string().c_str());
        }
        json.erase("requestType");
        json.erase("accountId");
        json.erase("targetId");
        json.erase("state");
        std::string webrtcPayload = boost::json::serialize(json);
        webrtcAsyncWrite(struct_pack::serialize<std::string>(webrtcEnvelope).append(webrtcPayload));
    }
}

void WebrtcManager::handleSystemAccept()
{
    boost::json::object registerJson;
    registerJson["requestType"] = static_cast<int>(WebrtcRequestState::REGISTER);
    registerJson["systemService"] = webrtcManagerConfig.systemService;
    registerJson["stunHost"]    = webrtcManagerConfig.stunHost;
    registerJson["turnHost"]    = webrtcManagerConfig.turnHost;
    registerJson["turnUsername"] = webrtcManagerConfig.turnUsername;
    registerJson["turnPassword"] = webrtcManagerConfig.turnPassword;
    registerJson["debugLog"]    = webrtcManagerConfig.webrtcDebugLog;
    registerJson["state"]       = 200;

    std::string registerStr = boost::json::serialize(registerJson);
    std::shared_ptr<WriterData> registerData = std::make_shared<WriterData>(registerStr.data(), registerStr.size());
    asyncWrite(registerData);

    if (!followData.empty()) {
        std::shared_ptr<WriterData> followDataPacket = std::make_shared<WriterData>(followData.data(), followData.size());
        asyncWrite(followDataPacket);
    }
}

void WebrtcManager::handleSystemDisconnect()
{
    if (onResetCursorHandle) onResetCursorHandle();

    cancelRequestTimeout();  // 本地 System 断开:取消挂起的看门狗

    bool wasRemote = isRemote.exchange(false);

    closeTcpSocket();

    releaseSource();

    initializePeerConnection();

    if (wasRemote) {

        if (onDisConnectRemoteHandle) onDisConnectRemoteHandle();

    } else {

        if (onRemoteFailedHandle) onRemoteFailedHandle();

    }
}

void WebrtcManager::sendSignalingMessage(boost::json::object& msg) {

    if (!webSocket || !webSocket->isOpen()) {
        LOG_ERROR("Cannot send signaling message - WebSocket not connected");
        if(onSignalServerDisConnectHandle){

            onSignalServerDisConnectHandle();

        }
        return;
    }

    // 业务载荷(msg)保持 JSON 文本,信封字段拆出;不修改 msg,也不往 body 里塞 state
    WebrtcEnvelope webrtcEnvelope;
    webrtcEnvelope.requestType = static_cast<int>(WebrtcRequestState::REQUEST);
    webrtcEnvelope.state = 200;   // 接收端按 state==200 处理
    webrtcEnvelope.accountId = accountId;
    if (!targetId.empty()) {
        webrtcEnvelope.targetId = targetId;
    }
    std::string webrtcPayload = boost::json::serialize(msg);

    try {
        webrtcAsyncWrite(struct_pack::serialize<std::string>(webrtcEnvelope).append(webrtcPayload));
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to send signaling message: %s",e.what());
    }
}

void WebrtcManager::closeTcpSocket()
{

    if (tcpSocket) {
        tcpSocket->closeEvent();
    }
}

void WebrtcManager::releaseSource()
{

    // 先唤醒解码线程再 Close:否则 Close() 内 Stop 等待解码线程,而解码线程
    // 卡在 renderCv.wait(等渲染设备注入)或 GPU 查询自旋,ioContext 线程被永久拖死。
    if (webrtcVideoDecoderFactory) {
        webrtcVideoDecoderFactory->wakeUpAllDecoders();
        webrtcVideoDecoderFactory->clearDecoderD3D11Device();
    }

    if (peerConnection) {
        peerConnection->Close();
    }

    if (dataChannel) {
        dataChannel->Close();
        dataChannel = nullptr;
    }

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

void WebrtcManager::armRequestTimeout(WebrtcRole role)
{
    bool caller = (role == WebrtcRole::Caller);
    int timeoutSeconds = caller ? 15 : 30;

    uint64_t epoch = ++timeoutEpoch;

    if (requestTimeout) {
        requestTimeout->cancel();  // 旧 timer 挂起的 async_wait 以 operation_aborted 返回
    }

    requestTimeout = std::make_shared<boost::asio::steady_timer>(ioContext);
    requestTimeout->expires_after(std::chrono::seconds(timeoutSeconds));

    auto timer = requestTimeout;

    boost::asio::co_spawn(ioContext,[self = shared_from_this(), timer, epoch, caller]()mutable->boost::asio::awaitable<void>{

        boost::system::error_code ec;

        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) co_return;  // 被新一轮请求刷新/取消

        if (epoch != self->timeoutEpoch) co_return;                   // 兜底:纪元对不上说明已过期

        if (!self->isRemote) {

            self->closeTcpSocket();

            self->releaseSource();

            self->initializePeerConnection();

            if (caller && self->onRemoteFailedHandle) self->onRemoteFailedHandle();

            LOG_INFO("WebrtcManager Request Timeout ReInit (caller=%d)", caller ? 1 : 0);

        }

    },boost::asio::detached);
}

void WebrtcManager::cancelRequestTimeout()
{

    ++timeoutEpoch;  // 使任何已挂起的看门狗协程按 epoch 不匹配退出

    if (requestTimeout) {
        requestTimeout->cancel();  // 挂起的 async_wait 以 operation_aborted 返回,协程 co_return
        requestTimeout.reset();
    }

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
            LOG_INFO("AsyncRemoteDesk: set decoder factory webrtcEnableD3D11=%d", self->webrtcDeskConfig.webrtcEnableD3D11);
        } else {
            LOG_WARN("AsyncRemoteDesk: webrtcVideoDecoderFactory is null, hard decode disabled");
        }

        if (self->targetId.empty()) {
            LOG_ERROR("TargetId not set");
            co_return;
        }

        if (!self->webSocket || !self->webSocket->isOpen()) {
            LOG_ERROR("WebSocket not connected");
            co_return;
        }

        if(self->peerConnection != nullptr){

            // 业务载荷(type=request + 桌面配置)保持 JSON 文本,信封字段拆出
            boost::json::object message;
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

            WebrtcEnvelope webrtcEnvelope;
            webrtcEnvelope.requestType = static_cast<int>(WebrtcRequestState::REQUEST);
            webrtcEnvelope.state = 200;   // 接收端按 state==200 处理
            webrtcEnvelope.accountId = self->accountId;
            webrtcEnvelope.targetId = self->targetId;
            std::string webrtcPayload = boost::json::serialize(message);

            bool enqueued = self->webrtcAsyncWrite(struct_pack::serialize<std::string>(webrtcEnvelope).append(webrtcPayload));

            if (enqueued) {
                LOG_INFO("AsyncRemoteDesk to Target: %s", self->targetId.c_str());
            } else {
                LOG_ERROR("AsyncRemoteDesk: request NOT enqueued, websocket send queue is closed");
            }

            self->armRequestTimeout(WebrtcRole::Caller);
        } else {

            LOG_ERROR("AsyncRemoteDesk: PeerConnection is null, request not sent");

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
    boost::asio::post(ioContext, [self = shared_from_this()]() {
        self->isRemote = false;
        self->cancelRequestTimeout();     // UI 超时重置:取消挂起的看门狗,避免与 onRemoteConnectionTimeout 双份 teardown
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
    if (webrtcVideoDecoderFactory) {
        webrtcVideoDecoderFactory->setOnDisplayHandle(onVideoFrameHandler);
    }
}

void WebrtcManager::disConnect()
{

    boost::asio::post(ioContext,[self = shared_from_this()](){

        self->cancelRequestTimeout();  // 彻底断开:取消挂起的看门狗

        if (self->webSocket) {

            self->webSocket->closeEvent();

            self->webSocket.reset();
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
