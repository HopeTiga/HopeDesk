#include "WebRTCManager.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <immintrin.h>
#include "ConfigManager.h"
#include "Logger.h"

namespace hope{

namespace rtc{

WebRTCManager::WebRTCManager(WebRTCRemoteState state)
    : state(state)
    , webSocket(nullptr)
    , connetState(WebRTCConnetState::none)
    ,channel(ioContext)
    ,accept(ioContext,boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"),19998))
    ,tcpSocket(nullptr)
    ,resolver(nullptr)
    ,ioContextWorkPtr(nullptr)
    ,writerChannel(ioContext)
{

    webSocket = std::make_unique<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>>(ioContext);

    ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    ioContextThread = std::move(std::thread([this](){
        this->ioContext.run();
    }));

    boost::asio::co_spawn(ioContext,[this]()->boost::asio::awaitable<void>{

        for(;;){

            std::unique_ptr<boost::asio::ip::tcp::socket> socket = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);

            co_await accept.async_accept(*socket,boost::asio::use_awaitable);

            tcpSocket = std::move(socket);

            Logger::getInstance()->info("tcpSocket Accept Successful!");

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

    if (!resolver) {
        resolver = std::make_unique<boost::asio::ip::tcp::resolver>(ioContext);
    }

    boost::asio::co_spawn(ioContext,[this,host,port]()->boost::asio::awaitable<void>{

        // 解析主机和端口
        auto results = co_await resolver->async_resolve(host, port, boost::asio::use_awaitable);

        if (!webSocket) {
            // 通知连接失败
            if (webSocketConnectedCallback) {
                webSocketConnectedCallback(false);
            }
            co_return;
        }

        co_await boost::asio::async_connect(webSocket->next_layer(), results, boost::asio::use_awaitable);

        // 执行 WebSocket 握手
        co_await webSocket->async_handshake(host+":"+port, "/",boost::asio::use_awaitable);


        webSocketRuns = true;

        if (webSocketConnectedCallback) {
            webSocketConnectedCallback(true);
        }

        boost::asio::co_spawn(ioContext, [this]() -> boost::asio::awaitable<void> {

            try{

                while (webSocketRuns) {

                    boost::beast::flat_buffer buffer;

                    co_await webSocket->async_read(buffer,boost::asio::use_awaitable);

                    dataStr = boost::beast::buffers_to_string(buffer.data());

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

                                                Logger::getInstance()->info("WindowsServiceManager::startService Successful!");

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
                                      Logger::getInstance()->error("webSocket Read coroutinePtr Error: " + std::string(e.what()));
                                      if(webSocketRuns){
                                          if(webSocketDisConnect){
                                              webSocketDisConnect(e);
                                          }
                                      }
                                  }
                              });

        boost::json::object request;

        request["requestType"] = static_cast<int>(WebRTCRequestState::REGISTER);

        request["accountID"] = this->accountID;

        std::string requestStr = boost::json::serialize(request);

        co_await webSocket->async_write(boost::asio::buffer(requestStr));

    },[this](std::exception_ptr ptr) {
                              // 正确的异常处理方式
                              if (ptr) { // 重要：检查是否确实有异常发生
                                  try {
                                      if (webSocketConnectedCallback) {
                                          webSocketConnectedCallback(false);
                                      }
                                      std::rethrow_exception(ptr); // 重新抛出异常
                                  }
                                  catch (const std::exception& e) {
                                      // 现在可以正常捕获并处理了
                                      LOG_ERROR("WebRTCManager Connect boost::asio::co_spawn Exception: %s", e.what());
                                  }
                              }
                          });


}

WebRTCManager::~WebRTCManager()
{
    WindowsServiceManager::stopService(systemService);

    releaseSource();

    networkThread.reset();

    workerThread.reset();

    signalingThread.reset();

    peerConnectionFactory.release();
}

bool WebRTCManager::initializePeerConnection()
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
        Logger::getInstance()->error("Failed to create PeerConnection: " + std::string(pcResult.error().message()));
        return false;
    }

    peerConnection = pcResult.MoveValue();
    return true;
}

void WebRTCManager::convertYUV420ToRGBA32(
    const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
    int width, int height, int yStride, int uStride, int vStride,
    uint8_t* rgbaData)
{
    // AVX2 constants for YUV to RGB conversion
    const __m256 v_to_r = _mm256_set1_ps(1.402f);
    const __m256 u_to_g = _mm256_set1_ps(0.344f);
    const __m256 v_to_g = _mm256_set1_ps(0.714f);
    const __m256 u_to_b = _mm256_set1_ps(1.772f);
    const __m256 offset = _mm256_set1_ps(128.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 max_val = _mm256_set1_ps(255.0f);
    const __m256i alpha_255 = _mm256_set1_epi32(0xFF000000);  // Alpha channel = 255

    for (int y = 0; y < height; y++) {
        int x = 0;

        // Process 8 pixels at a time with AVX2
        for (; x <= width - 8; x += 8) {
            // Load Y values (8 bytes)
            __m128i y8 = _mm_loadl_epi64((__m128i*)(yData + y * yStride + x));
            __m256i y32 = _mm256_cvtepu8_epi32(y8);
            __m256 yf = _mm256_cvtepi32_ps(y32);

            // Load U and V values (4 values each for YUV420, expanded to 8)
            alignas(32) uint8_t u_vals[8], v_vals[8];
            int uvRow = y / 2;
            for (int i = 0; i < 8; i++) {
                int uvCol = (x + i) / 2;
                u_vals[i] = uData[uvRow * uStride + uvCol];
                v_vals[i] = vData[uvRow * vStride + uvCol];
            }

            __m128i u8 = _mm_loadl_epi64((__m128i*)u_vals);
            __m128i v8 = _mm_loadl_epi64((__m128i*)v_vals);
            __m256i u32 = _mm256_cvtepu8_epi32(u8);
            __m256i v32 = _mm256_cvtepu8_epi32(v8);
            __m256 uf = _mm256_cvtepi32_ps(u32);
            __m256 vf = _mm256_cvtepi32_ps(v32);

            // Adjust U and V
            uf = _mm256_sub_ps(uf, offset);
            vf = _mm256_sub_ps(vf, offset);

            // Calculate RGB
            __m256 rf = _mm256_fmadd_ps(vf, v_to_r, yf);
            __m256 gf = _mm256_fnmadd_ps(uf, u_to_g, yf);
            gf = _mm256_fnmadd_ps(vf, v_to_g, gf);
            __m256 bf = _mm256_fmadd_ps(uf, u_to_b, yf);

            // Clamp to 0-255
            rf = _mm256_max_ps(zero, _mm256_min_ps(max_val, rf));
            gf = _mm256_max_ps(zero, _mm256_min_ps(max_val, gf));
            bf = _mm256_max_ps(zero, _mm256_min_ps(max_val, bf));

            // Convert to int32
            __m256i ri = _mm256_cvtps_epi32(rf);
            __m256i gi = _mm256_cvtps_epi32(gf);
            __m256i bi = _mm256_cvtps_epi32(bf);

            // 直接提取并存储，避免复杂的打包操作
            alignas(32) int32_t r[8], g[8], b[8];
            _mm256_store_si256((__m256i*)r, ri);
            _mm256_store_si256((__m256i*)g, gi);
            _mm256_store_si256((__m256i*)b, bi);

            // 直接写入RGBA数据
            uint8_t* dst = rgbaData + (y * width + x) * 4;
            for (int i = 0; i < 8; i++) {
                dst[i*4 + 0] = (uint8_t)r[i];  // R
                dst[i*4 + 1] = (uint8_t)g[i];  // G
                dst[i*4 + 2] = (uint8_t)b[i];  // B
                dst[i*4 + 3] = 255;             // A
            }
        }

        // Handle remaining pixels with scalar code
        for (; x < width; x++) {
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

            int rgbaIndex = (y * width + x) * 4;
            rgbaData[rgbaIndex] = R;
            rgbaData[rgbaIndex + 1] = G;
            rgbaData[rgbaIndex + 2] = B;
            rgbaData[rgbaIndex + 3] = 255;
        }
    }
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

    if(webSocket && webSocket->is_open()){
        boost::json::object message;
        message["accountID"] = this->accountID;
        message["targetID"] = this->targetID;
        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);
        std::string messageStr = boost::json::serialize(message);
        webSocket->write(boost::asio::buffer(messageStr));
    }

}

void WebRTCManager::disConnectHandle()
{
    SystemParametersInfo(SPI_SETCURSORS,0,NULL,0);

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

void WebRTCManager::setSystemServiceExe(std::string webrtcExe)
{
    this->systemServiceExe = webrtcExe;
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

            if(webSocket && webSocket->is_open()){
                webSocket->write(boost::asio::buffer(bodyStr));
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
                                  Logger::getInstance()->error("Reader coroutine handler exception: " + std::string(e.what()));
                              }
                          });
}

void WebRTCManager::sendSignalingMessage(boost::json::object& msg) {
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
        webSocket->async_write(boost::asio::buffer(messageStr),boost::asio::detached);
    } catch (const std::exception& e) {
        Logger::getInstance()->error("Failed to send signaling message: " + std::string(e.what()));
    }
}

void WebRTCManager::handleAsioException()
{

    this->state = WebRTCRemoteState::nullRemote;

    isRemote = false;

    if(webSocket && webSocket->is_open()){

        boost::json::object message;

        message["accountID"] = this->accountID;

        message["targetID"] = this->targetID;

        message["requestType"] = static_cast<int64_t>(WebRTCRequestState::RESTART);

        std::string messageStr = boost::json::serialize(message);

        webSocket->async_write(boost::asio::buffer(messageStr),boost::asio::detached);

        WindowsServiceManager::stopService(systemService);


    }
}

void WebRTCManager::releaseSource()
{

    if (videoTrack && videoSinkImpl) {
        auto videoTrack = static_cast<webrtc::VideoTrackInterface*>(this->videoTrack.get());
        videoTrack->RemoveSink(videoSinkImpl.get());

        videoSinkImpl.reset();
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

std::string WebRTCManager::getAccountID() const
{
    return accountID;
}

void WebRTCManager::setAccountID(const std::string &newAccountID)
{
    accountID = newAccountID;
}

void WebRTCManager::sendRequestToTarget()
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

        webSocket->async_write(boost::asio::buffer(boost::json::serialize(message)),boost::asio::detached);

        LOG_INFO("Request sent to target: %s", targetID.c_str());
    }


}

std::string WebRTCManager::getTargetID() const
{
    return targetID;
}

void WebRTCManager::setTargetID(const std::string &newTargetID)
{
    targetID = newTargetID;
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
    }
}

void WebRTCManager::setVideoFrameCallback(VideoFrameCallback callback)
{
    this->videoFrameCallback = callback;
}

void WebRTCManager::disConnect()
{
    webSocketRuns = false;

    if (webSocket && webSocket->is_open()) {
        try{
            webSocket->next_layer().cancel();
            webSocket->async_close(boost::beast::websocket::close_code::normal,boost::asio::detached);
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

    disConnectHandle();

    isRemote = false;

}


}

}
