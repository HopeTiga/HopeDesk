#include "WebrtcManager.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <api/video/i420_buffer.h>
#include <api/video/nv12_buffer.h>
#include <api/field_trials.h>
#include <third_party/libyuv/libyuv.h>

#include "buffer/WebrtcI420Buffer.h"
#include "buffer/WebrtcNv12Buffer.h"
#include "buffer/WebrtcD3D11TextureBuffer.h"


namespace hope {

    namespace rtc {

        WebrtcManager::WebrtcManager(std::function<void()> closeHandle, WebrtcDeskSystemConfig config)
            : ioContext(1)
            , tcpSocket(std::make_shared<TcpSocket>(ioContext))
            , closeHandle(closeHandle)
            , peerConnection(nullptr)
            , winLogon(nullptr)
            , keyMouseSim(nullptr)
            , webrtcDeskSystemConfig(config)
            , bufferPool(false, 100)
            , cursorHooks(nullptr)
            , screenCapture(nullptr)
            , hAudioCatch(nullptr) {

            // 共享通道同步状态：下游编码器与上游捕获通过它互相通知（VDD 重建自愈）。
            channelSync = std::make_shared<VddChannelSync>();

            ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
                boost::asio::make_work_guard(ioContext));

            ioContextThread = std::move(std::thread([this]() {
                this->ioContext.run();
                }));

            tcpSocket->setOnMessageHandle([this](std::string message) {
                handleSystemMessage(std::move(message));
            });

            tcpSocket->setOnDisConnectHandle([this]() {
                if (this->closeHandle) {
                    this->closeHandle();
                }
            });

            boost::asio::co_spawn(ioContext, [this]()-> boost::asio::awaitable<void> {
                co_await tcpSocket->connect(19998);
                co_return;
            }, boost::asio::detached);

                keyMouseSim = std::make_unique<KeyMouseSimulator>();

                if (!keyMouseSim->Initialize()) {
                    LOG_ERROR("KeyMouseSimulator initialization failed");
                }

        }

        void WebrtcManager::sendSignalingMessage(const boost::json::object& message) {

            boost::json::object fullMsg;
            fullMsg["requestType"] = static_cast<int64_t>(WebrtcRequestState::REQUEST);
            fullMsg["accountId"] = accountId;
            fullMsg["targetId"] = targetId;
            fullMsg["state"] = 200;

            for (auto& [key, value] : message) {
                fullMsg[key] = value;
            }

            std::string msgStr = boost::json::serialize(fullMsg);
            auto data = std::make_shared<WriterData>(const_cast<char*>(msgStr.c_str()), msgStr.size());

            asyncWrite(data);
        }

        void WebrtcManager::processOffer(const std::string& sdp) {
            if (sdp.empty()) {
                LOG_ERROR("Received empty SDP offer");
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
                LOG_ERROR("Failed to parse offer: %s", error.description);
            }
        }

        void WebrtcManager::processAnswer(const std::string& sdp) {
            if (sdp.empty()) {
                LOG_ERROR("Received empty SDP answer");
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
                LOG_ERROR("Failed to parse answer: %s", error.description.c_str());
            }
        }

        void WebrtcManager::processIceCandidate(const std::string& candidate,
            const std::string& mid, int lineIndex) {
            if (candidate.empty()) {
                return;
            }

            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::IceCandidateInterface> iceCandidate(
                webrtc::CreateIceCandidate(mid, lineIndex, candidate, &error));

            if (iceCandidate) {
                peerConnection->AddIceCandidate(iceCandidate.release());
            }
            else {
                LOG_ERROR("Failed to parse ICE candidate: %s", error.description.c_str());
            }
        }

        void WebrtcManager::asyncWrite(std::shared_ptr<WriterData> data) {

            if (!data) {

                return;

            }

            tcpSocket->asyncWrite(std::move(data));

        }

        void WebrtcManager::post(std::function<void()> task)
        {
            boost::asio::post(ioContext, std::move(task));
        }

        bool WebrtcManager::initializePeerConnection() {
            // Clean up any existing connection first
            if (peerConnection) {

                releaseSource();

            }

            webrtc::InitializeSSL();

            if (!peerConnectionFactory) {

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

                networkThread->PostTask([this]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    });

                workerThread->PostTask([this]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    });

                audioDeviceModuleImpl = AudioDeviceModuleImpl::Create();

                std::unique_ptr<WebrtcVideoEncoderFactory> webrtcVideoEncoderFactoryUnique = std::make_unique<WebrtcVideoEncoderFactory>();

                webrtcVideoEncoderFactory = webrtcVideoEncoderFactoryUnique.get();

                std::unique_ptr<WebrtcVideoDecoderFactory> webrtcVideoDecoderFactoryUnique = std::make_unique<WebrtcVideoDecoderFactory>();

                webrtcVideoDecoderFactory = webrtcVideoDecoderFactoryUnique.get();

                peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
                    networkThread.get(),
                    workerThread.get(),
                    signalingThread.get(),
                    audioDeviceModuleImpl,
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

            config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;

            config.ice_connection_receiving_timeout = 10000;        // 连上后 10s 收不到数据才判断开(检测断开保持 10s)

            config.ice_unwritable_timeout = 30000;                  // 建立阶段 30s 无写成功才标记不可写(放宽,避免慢连被误杀)

            config.ice_inactive_timeout = 30000;                    // 30s 无活动才标记非活跃(放宽,配合慢启动)

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

                LOG_ERROR("Failed to create PeerConnection: %s", pcResult.error().message());

                return false;

            }

            peerConnection = pcResult.MoveValue();

            videoTrackSourceImpl = webrtc::make_ref_counted<VideoTrackSourceImpl>();

            videoTrack = peerConnectionFactory->CreateVideoTrack(videoTrackSourceImpl, "videoTrack");

            if (!videoTrack) {

                LOG_ERROR("Failed to create video track");

                return false;

            }

            videoTrack->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);

            std::vector<webrtc::RtpEncodingParameters> encodings;

            int maxBitrateBps = webrtcDeskSystemConfig.localMaxBitrateBps;

            int minBitrateBps = webrtcDeskSystemConfig.localMinBitrateBps;

            int maxFramerate = webrtcDeskSystemConfig.localMaxFramerate;

            if (webrtcDeskSystemConfig.localMaxBitrateBps >= webrtcDeskSystemConfig.requestMaxBitrateBps) {
            
                maxBitrateBps = webrtcDeskSystemConfig.requestMaxBitrateBps;

            }

            if (webrtcDeskSystemConfig.localMinBitrateBps >= webrtcDeskSystemConfig.requestMinBitrateBps) {

                minBitrateBps = webrtcDeskSystemConfig.requestMinBitrateBps;

            }

            if (webrtcDeskSystemConfig.localMaxFramerate >= webrtcDeskSystemConfig.requestMaxFramerate) {

                maxFramerate = webrtcDeskSystemConfig.requestMaxFramerate;

            }

            if (minBitrateBps > maxBitrateBps) {

                minBitrateBps = maxBitrateBps;

            }

            webrtc::RtpEncodingParameters encoding = getDefaultRtpEncodingParameters();

            encoding.max_bitrate_bps = maxBitrateBps;

            encoding.min_bitrate_bps = minBitrateBps;

            encoding.max_framerate = maxFramerate;

            encodings.push_back(encoding);

            std::vector<std::string> streamIds = { "mediaStream" };

            webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> videoTrackResult = peerConnection->AddTrack(videoTrack, streamIds, encodings);

            if (!videoTrackResult.ok()) {

                LOG_ERROR("Failed to add video track: %s", videoTrackResult.error().message());

                return false;

            }

            videoSender = videoTrackResult.MoveValue();

            auto transceivers = peerConnection->GetTransceivers();

            for (auto& transceiver : transceivers) {

                if (transceiver->media_type() == webrtc::MediaType::VIDEO) {

                    webrtc::RtpCapabilities senderCapabilities = peerConnectionFactory->GetRtpSenderCapabilities(
                        webrtc::MediaType::VIDEO);

                    senderCapabilities.fec.clear();

                    if (senderCapabilities.codecs.empty()) {

                        LOG_WARN("No video codecs available from factory");

                        continue;

                    }

                    std::vector<webrtc::RtpCodecCapability> preferredCodecs;
                    // 根据枚举选择优先编解码器
                    std::string priorityCodec;

                    switch (webrtcDeskSystemConfig.videoCodec) {

                    case WebrtcVideoCodec::VP9: priorityCodec = "VP9"; break;

                    case WebrtcVideoCodec::H264: priorityCodec = "H264"; break;

                    case WebrtcVideoCodec::VP8: priorityCodec = "VP8"; break;

                    case WebrtcVideoCodec::H265: priorityCodec = "H265"; break;

                    case WebrtcVideoCodec::AV1: priorityCodec = "AV1"; break;

                    }

                    LOG_INFO("Attempting to prioritize codec: %s", priorityCodec.c_str());

                    // 首先添加优先编解码器
                    bool foundPriorityCodec = false;

                    for (const auto& codec : senderCapabilities.codecs) {

                        if (codec.name == priorityCodec) {

                            preferredCodecs.push_back(codec);

                            foundPriorityCodec = true;

                            LOG_INFO("Found and prioritized codec: %s", codec.name.c_str());

                            break;
                        }
                    }

                    if (!foundPriorityCodec) {

                        LOG_WARN("Priority codec %s not found in available codecs", priorityCodec.c_str());

                    }

                    // 添加其他可用编解码器（排除重复项和辅助编解码器）
                    for (const auto& codec : senderCapabilities.codecs) {

                        if (codec.name != priorityCodec) {

                            preferredCodecs.push_back(codec);

                            LOG_INFO("Added additional codec: %s", codec.name.c_str());

                        }
                    }

                    // 验证是否有编解码器可设置
                    if (preferredCodecs.empty()) {

                        LOG_ERROR("No valid codecs to set as preferences");

                        continue;

                    }

                    // 设置编解码器偏好
                    auto result = transceiver->SetCodecPreferences(preferredCodecs);

                    if (result.ok()) {

                        LOG_INFO("Successfully set codec preferences with %d codecs", preferredCodecs.size());

                    }
                    else {

                        LOG_ERROR("Failed to set codec preferences: %s", result.message());

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
                LOG_ERROR("Failed to set RTP parameters: %s", setParamsResult.message());
                return false;
            }

            if (webrtcDeskSystemConfig.webrtcAudioEnable == 1) {

                audioTrack = peerConnectionFactory->CreateAudioTrack("audioTrack", nullptr);

                std::vector<std::string> audioStreamIds = { "audioStream" };

                webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> audioTrackResult = peerConnection->AddTrack(audioTrack, audioStreamIds);

                if (!audioTrackResult.ok()) {

                    LOG_ERROR("Failed to add video track: %s", audioTrackResult.error().message());

                    return false;

                }

                audioSender = audioTrackResult.MoveValue();

            }

            std::unique_ptr<webrtc::DataChannelInit> dataChannelConfig = std::make_unique<webrtc::DataChannelInit>();

            dataChannelConfig->priority = webrtc::PriorityValue(webrtc::Priority::kHigh);

            webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> dataChannelResult = peerConnection->CreateDataChannelOrError("dataChannel", dataChannelConfig.get());

            if (!dataChannelResult.ok()) {
            
                LOG_ERROR("Failed to add dataChannel: %s", dataChannelResult.error().message());

                return false;

            }

			dataChannel = dataChannelResult.MoveValue();

            dataChannelObserver = std::make_unique<DataChannelObserverImpl>();

            dataChannelObserver->setOnDataHandle(std::bind(&WebrtcManager::handleDataChannelData, this, std::placeholders::_1, std::placeholders::_2));

            dataChannel->RegisterObserver(dataChannelObserver.get());

            return true;
        }

        bool WebrtcManager::initializeScreenCapture() {
            if (screenCapture || compatScreenCapture) {
                return false;
            }

            // webrtcModulesType: 0=游戏模式(高性能 VDD) 1=办公模式(兼容 ScreenCapture)
            if (webrtcDeskSystemConfig.webrtcModulesType == 1) {
                // === 兼容模式：ScreenCapture (DXGI 桌面采集)，CPU 传输，不用脏矩形 ===
                LOG_INFO("webrtcModulesType=1 (办公模式): 兼容模式 (ScreenCapture)");
                activeCaptureTech = "Desktop Duplication API";
                auto sc = std::make_shared<ScreenCapture>();
                ScreenCapture::CaptureConfig cfg;
                cfg.width = webrtcDeskSystemConfig.desktopWidth > 0 ? webrtcDeskSystemConfig.desktopWidth : 1920;
                cfg.height = webrtcDeskSystemConfig.desktopHeight > 0 ? webrtcDeskSystemConfig.desktopHeight : 1080;
                // 采集层级直接用客户端传的 webrtcUseLevels: 0=CPU 1=GPU 2=PRO
                const int useLevel = webrtcDeskSystemConfig.webrtcUseLevels;
                const CaptureLevels captureLevel = (useLevel >= 0 && useLevel <= 2)
                    ? static_cast<CaptureLevels>(useLevel) : CaptureLevels::CPU;
                cfg.levels = captureLevel;
                cfg.uselevels = captureLevel;
                cfg.enableDirtyRects = false;                      // 不用脏矩形
                sc->setConfig(cfg);

                // ScreenCapture 输出格式: CPU=BGRA, GPU=I420, PRO=NV12 -> 统一转 I420 后 PushFrame
                sc->setDataHandle([this](const uint8_t* data, int width, int height,
                    std::atomic<bool>* isBusy, int rowPitch, CaptureLevels level) {
                        if (!videoTrackSourceImpl || !data) {
                            if (isBusy) isBusy->store(false);
                            return;
                        }
                        webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer = bufferPool.CreateI420Buffer(width, height);
                        if (level == CaptureLevels::CPU) {
                            // CPU: BGRA -> I420
                            libyuv::ARGBToI420(
                                data, rowPitch,
                                i420Buffer->MutableDataY(), i420Buffer->StrideY(),
                                i420Buffer->MutableDataU(), i420Buffer->StrideU(),
                                i420Buffer->MutableDataV(), i420Buffer->StrideV(),
                                width, height);
                        }
                        else if (level == CaptureLevels::GPU) {
                            // GPU 级: 计算着色器输出 I420 (Y/U/V 三平面, U/V 行距=rowPitch/2, 见 ScreenCapture 注释)
                            const uint8_t* src_y = data;
                            const uint8_t* src_u = data + static_cast<size_t>(rowPitch) * height;
                            const uint8_t* src_v = src_u + static_cast<size_t>(rowPitch / 2) * (height / 2);
                            libyuv::I420Copy(
                                src_y, rowPitch,
                                src_u, rowPitch / 2,
                                src_v, rowPitch / 2,
                                i420Buffer->MutableDataY(), i420Buffer->StrideY(),
                                i420Buffer->MutableDataU(), i420Buffer->StrideU(),
                                i420Buffer->MutableDataV(), i420Buffer->StrideV(),
                                width, height);
                        }
                        else {
                            // PRO 级: NV12 -> I420 (UV 平面紧跟 Y 平面, 行距=rowPitch)
                            libyuv::NV12ToI420(
                                data, rowPitch,
                                data + static_cast<size_t>(rowPitch) * height, rowPitch,
                                i420Buffer->MutableDataY(), i420Buffer->StrideY(),
                                i420Buffer->MutableDataU(), i420Buffer->StrideU(),
                                i420Buffer->MutableDataV(), i420Buffer->StrideV(),
                                width, height);
                        }
                        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                            .set_video_frame_buffer(i420Buffer)
                            .set_rotation(webrtc::kVideoRotation_0)
                            .set_timestamp_us(webrtc::TimeMicros())
                            .build();
                        videoTrackSourceImpl->PushFrame(frame);
                        if (isBusy) isBusy->store(false);
                    });

                compatScreenCapture = sc;
                if (!sc->initialize()) {
                    LOG_ERROR("Failed to initialize ScreenCapture (compat mode)");
                    compatScreenCapture.reset();
                    return false;
                }
                if (!sc->startCapture()) {
                    LOG_ERROR("Failed to start ScreenCapture (compat mode)");
                    compatScreenCapture.reset();
                    return false;
                }
            }
            else {
                // === 高性能模式：VirtualDisplayCapture (VDD 帧通道) ===
                screenCapture = std::make_shared<VirtualDisplayCapture>();
                activeCaptureTech = "Hope Virtual Display";

                hope::rtc::VirtualDisplayCapture::Config config;
                // 分辨率/刷新率来自客户端配置(WebrtcDeskSystemConfig)，独立于 WebRTC 帧率。
                config.width = webrtcDeskSystemConfig.desktopWidth > 0 ? webrtcDeskSystemConfig.desktopWidth : 1920;
                config.height = webrtcDeskSystemConfig.desktopHeight > 0 ? webrtcDeskSystemConfig.desktopHeight : 1080;
                config.refreshRate = webrtcDeskSystemConfig.desktopRefreshRate > 0
                    ? webrtcDeskSystemConfig.desktopRefreshRate : 144;
                config.bitsPerChannel = 8;
                config.hdrMode = 0;
                // Find-or-create a persistent display keyed by the systemService id:
                // if a display for this service already exists, reuse it instead of adding one.
                config.id = webrtcManagerConfig.systemService.empty()
                    ? "HopeDesk" : webrtcManagerConfig.systemService.c_str();
                config.name = "HopeDeskSystem";
                config.removeOnDestroy = false;

                if (webrtcDeskSystemConfig.webrtcEnableNvenc == 1) {
                    LOG_INFO("webrtcEnableNvenc");
                    config.cpuPath = false; // zero-copy GPU shared handle -> NVENC direct injection

                    screenCapture->setGpuDataHandle([this](HANDLE sharedHandle,
                        int width, int height,
                        UINT format, UINT rowPitch,
                        UINT64 frameId
                        ) {
                            if (!videoTrackSourceImpl || !sharedHandle) {
                                return;
                            }

                            // NVENC direct-injection only reads GetSharedHandle(); releaseFlag unused
                            // (pass nullptr - keyed-mutex sync is handled in the encoder).
                            webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
                                webrtc::make_ref_counted<WebrtcD3D11TextureBuffer>(sharedHandle, width, height, nullptr);

                            webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                .set_video_frame_buffer(buffer)
                                .set_rotation(webrtc::kVideoRotation_0)
                                .set_timestamp_us(webrtc::TimeMicros())
                                .build();

                            videoTrackSourceImpl->PushFrame(frame);

                        });

                }
                else {
                    config.cpuPath = true; // mapped BGRA CPU buffer -> libyuv ARGBToI420

                    screenCapture->setDataHandle([this](const uint8_t* data, int width, int height, int stride, UINT64 frameId) {

                        if (!videoTrackSourceImpl || !data) {
                            return;
                        }

                        webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer = bufferPool.CreateI420Buffer(width, height);
                        // VirtualDisplayCapture CPU path delivers BGRA; stride is the mapped row pitch.
                        libyuv::ARGBToI420(
                            data, stride,
                            i420Buffer->MutableDataY(), i420Buffer->StrideY(),
                            i420Buffer->MutableDataU(), i420Buffer->StrideU(),
                            i420Buffer->MutableDataV(), i420Buffer->StrideV(),
                            width, height
                        );

                        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                            .set_video_frame_buffer(i420Buffer)
                            .set_rotation(webrtc::kVideoRotation_0)
                            .set_timestamp_us(webrtc::TimeMicros())
                            .build();

                        videoTrackSourceImpl->PushFrame(frame);
                    });
                }

                screenCapture->setConfig(config);

                // 与下游编码器共享通道同步状态：编码器 keyed-mutex 同步丢失时
                // 请求本捕获重开帧通道自愈。
                screenCapture->setChannelSync(channelSync);

                if (!screenCapture->initialize()) {
                    LOG_ERROR("Failed to initialize screen capture");
                    return false;
                }

                if (!screenCapture->startCapture()) {
                    LOG_ERROR("Failed to start screen capture");
                    return false;
                }

            }

            // 光标上报（两个模式共用）
            cursorHooks = std::make_unique<CursorHooks>();

            cursorHooks->setCursorHandle([this](unsigned char* data, size_t size) {

                if (!dataChannel) {

                    delete[] data;

                    return;

                }

                webrtc::CopyOnWriteBuffer buffer(data, size);

                webrtc::DataBuffer dataBuffer(buffer, true); // true 表示二进制数据

                dataChannel->SendAsync(dataBuffer, [this, data](webrtc::RTCError) {

                    delete[] data;

                    });

                });

            cursorHooks->startHooks();

            return true;
        }

        bool WebrtcManager::initializeHAudioCatch()
        {

            hAudioCatch = std::make_shared<HAudioCatch>();

            hAudioCatch->setDataHandle([this](unsigned char* data, size_t size) {

                if (!data) return;

                if (!audioDeviceModuleImpl) return;

                audioDeviceModuleImpl->PushAudioData(data, size);

                });

            if (!hAudioCatch->initlize()) {

                LOG_ERROR("HAudioCatch Initlize Failed!");

            }

            if (!hAudioCatch->runEventLoop()) {

                LOG_ERROR("HAudioCatch Run Event Loop Failed!");

                return false;

            }

            return true;
        }

        void WebrtcManager::handleDataChannelData(const unsigned char* data, size_t size)
        {

            if (size < sizeof(short)) {
                return;
            }

            const short eventType = *reinterpret_cast<const short*>(data);

            // 缓存屏幕分辨率
            thread_local static const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            thread_local static const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            switch (eventType) {
            case 0: { // Mouse move
                if (size < sizeof(short) + 2 * sizeof(uint32_t)) return;

#pragma pack(push,1)
                struct MouseMove              // 6 字节
                {
                    short  type;              // 0
                    uint32_t x;               // 屏幕绝对像素
                    uint32_t y;
                };
#pragma pack(pop)

                const MouseMove* mouseMove = reinterpret_cast<const MouseMove*>(data);

                keyMouseSim->MouseMove(mouseMove->x, mouseMove->y, true, true);

                break;
            }

            case 6: { // Mouse relative move (游戏视角，相对增量)
                if (size < sizeof(short) + 2 * sizeof(uint32_t)) return;

#pragma pack(push,1)
                struct MouseMove              // 6 字节
                {
                    short  type;              // 6
                    uint32_t x;               // dx (按 int32 解释)
                    uint32_t y;               // dy
                };
#pragma pack(pop)

                const MouseMove* mouseMove = reinterpret_cast<const MouseMove*>(data);

                int dx = static_cast<int32_t>(mouseMove->x);
                int dy = static_cast<int32_t>(mouseMove->y);

                keyMouseSim->MouseMove(dx, dy, false);

                break;
            }

            case 1:  // Mouse button down
            case 2: { // Mouse button up
                constexpr std::size_t kMinSize = sizeof(short) * 2 + 2 * sizeof(int);
                if (size < kMinSize) return;

                // 一次取完所有数据
                const auto* p = reinterpret_cast<const int16_t*>(data);
                const int16_t  mouseType = p[1];               // 2 字节
                const int32_t* coordPtr = reinterpret_cast<const int32_t*>(p + 2); // 8 字节

                // 哨兵 0xFFFFFFFF：相对模式下按键不带坐标，跳过绝对定位
                // (避免把游戏已锁定的光标 warp 到角落)
                if (coordPtr[0] == 0xFFFFFFFFu && coordPtr[1] == 0xFFFFFFFFu) {
                    if (eventType == 1)
                        keyMouseSim->MouseButtonDown(mouseType, -1, -1);
                    else
                        keyMouseSim->MouseButtonUp(mouseType);
                    break;
                }

                // 0-65535 固定点 → 屏幕坐标，一次 64-bit 乘
                const uint32_t scaleX = (static_cast<uint64_t>(coordPtr[0]) * screenWidth) >> 16;
                const uint32_t scaleY = (static_cast<uint64_t>(coordPtr[1]) * screenHeight) >> 16;

                if (eventType == 1)
                    keyMouseSim->MouseButtonDown(mouseType, scaleX, scaleY);
                else
                    keyMouseSim->MouseButtonUp(mouseType);
                break;
            }

            case 3: // Key down
            case 4: { // Key up
                if (size < sizeof(short) + 2) {
                    return;
                }

                const unsigned char* keyData = data + sizeof(short);

                if (eventType == 3) {
                    keyMouseSim->KeyDown(keyData[0], keyData[1]);
                }
                else {
                    keyMouseSim->KeyUp(keyData[0], keyData[1]);
                }
                break;
            }

            case 5: { // Mouse wheel
                if (size < sizeof(short) + sizeof(int)) {
                    return;
                }

                keyMouseSim->MouseWheel(*reinterpret_cast<const int*>(data + sizeof(short)));
                break;
            }

            case 7: {
            
                cursorHooks->clearCursorCache();

            }

            default:
                break;
            }
        }

        void WebrtcManager::handleSystemMessage(std::string message)
        {
            boost::json::object json;
            try {
                json = boost::json::parse(message).as_object();
            }
            catch (const std::exception& e) {
                LOG_ERROR("Json parse error: %s", e.what());
                return;
            }

            if (!json.contains("requestType")) return;

            int64_t requestType = json["requestType"].as_int64();

            if (!json.contains("state")) return;

            int64_t responseState = json["state"].as_int64();

            if (WebrtcRequestState(requestType) == WebrtcRequestState::REGISTER)
            {
                if (json.contains("systemService")) {
                    webrtcManagerConfig.systemService = json["systemService"].as_string().c_str();
                }
                if (json.contains("stunHost")) {
                    webrtcManagerConfig.stunHost = json["stunHost"].as_string().c_str();
                }
                if (json.contains("turnHost")) {
                    webrtcManagerConfig.turnHost = json["turnHost"].as_string().c_str();
                }
                if (json.contains("turnUsername")) {
                    webrtcManagerConfig.turnUsername = json["turnUsername"].as_string().c_str();
                }
                if (json.contains("turnPassword")) {
                    webrtcManagerConfig.turnPassword = json["turnPassword"].as_string().c_str();
                }

                if (json.contains("debugLog")) {
                    bool debugLog = json["debugLog"].as_bool();
                    if (debugLog) {
                        LOG_INFO("Enable Webrtc DebugLog");
                        initWebrtcLogging();
                    } else {
                        closeWebrtcLogging();
                    }
                }
            }
            else if (WebrtcRequestState(requestType) == WebrtcRequestState::REQUEST) {
                if (responseState == 200) {
                    if (json.contains("type")) {
                        std::string type(json["type"].as_string().c_str());

                        if (type == "request") {
                            if (json.contains("codec")) {
                                webrtcDeskSystemConfig.videoCodec = static_cast<WebrtcVideoCodec>(json["codec"].as_int64());
                            }

                            if (json.contains("webrtcAudioEnable")) {
                                webrtcDeskSystemConfig.webrtcAudioEnable = json["webrtcAudioEnable"].as_int64();
                            }

                            if (json.contains("webrtcEnableNvenc")) {
                                webrtcDeskSystemConfig.webrtcEnableNvenc = json["webrtcEnableNvenc"].as_int64();
                            }

                            if (json.contains("localMaxBitrateBps")) {
                                webrtcDeskSystemConfig.localMaxBitrateBps = json["localMaxBitrateBps"].as_int64();
                            }

                            if (json.contains("localMinBitrateBps")) {
                                webrtcDeskSystemConfig.localMinBitrateBps = json["localMinBitrateBps"].as_int64();
                            }

                            if (json.contains("localMaxFramerate")) {
                                webrtcDeskSystemConfig.localMaxFramerate = json["localMaxFramerate"].as_int64();
                            }

                            if (json.contains("desktopWidth")) {
                                webrtcDeskSystemConfig.desktopWidth = static_cast<int>(json["desktopWidth"].as_int64());
                            }

                            if (json.contains("desktopHeight")) {
                                webrtcDeskSystemConfig.desktopHeight = static_cast<int>(json["desktopHeight"].as_int64());
                            }

                            if (json.contains("desktopRefreshRate")) {
                                webrtcDeskSystemConfig.desktopRefreshRate = static_cast<int>(json["desktopRefreshRate"].as_int64());
                            }

                            if (json.contains("requestMaxBitrateBps")) {
                                webrtcDeskSystemConfig.requestMaxBitrateBps = json["requestMaxBitrateBps"].as_int64();
                            }

                            if (json.contains("requestMinBitrateBps")) {
                                webrtcDeskSystemConfig.requestMinBitrateBps = json["requestMinBitrateBps"].as_int64();
                            }

                            if (json.contains("requestMaxFramerate")) {
                                webrtcDeskSystemConfig.requestMaxFramerate = json["requestMaxFramerate"].as_int64();
                            }

                            if (!initializePeerConnection()) {
                                LOG_ERROR("Failed to initialize peer connection");
                                return;
                            }

                            if (webrtcVideoEncoderFactory) {
                                webrtcVideoEncoderFactory->webrtcEnableNvenc = webrtcDeskSystemConfig.webrtcEnableNvenc;

                                // 注入 VDD 通道同步状态：编码器检测到 keyed-mutex 同步丢失时
                                // 请求上游重开帧通道自愈。
                                webrtcVideoEncoderFactory->channelSync = channelSync;

                                webrtcVideoEncoderFactory->onEncoderStatusHandle =
                                    [this](const std::string& codec, bool hardEncode) {
                                    boost::json::object o;
                                    o["requestType"] = static_cast<int64_t>(WebrtcRequestState::ENCODE_STATUS);
                                    o["codec"] = codec;
                                    o["hard"] = hardEncode;
                                    o["capture"] = activeCaptureTech;  // "Hope Virtual Display" / "Desktop Duplication API"
                                    std::string s = boost::json::serialize(o);
                                    auto data = std::make_shared<WriterData>(const_cast<char*>(s.data()), s.size());
                                    asyncWrite(data);
                                };
                            }

                            if (json.contains("webrtcModulesType")) {
                                webrtcDeskSystemConfig.webrtcModulesType = json["webrtcModulesType"].as_int64();
                            }

                            if (json.contains("webrtcUseLevels")) {
                                webrtcDeskSystemConfig.webrtcUseLevels = json["webrtcUseLevels"].as_int64();
                            }

                            if (!initializeScreenCapture()) {
                                LOG_ERROR("Failed to initialize ScreenCapture");
                                return;
                            }

                            if (webrtcDeskSystemConfig.webrtcAudioEnable == 1) {
                                if (!initializeHAudioCatch()) {
                                    LOG_ERROR("Failed to initialize HAudioCatch");
                                    return;
                                }
                            }

                            if (json.contains("accountId")) {
                                targetId = std::string(json["accountId"].as_string().c_str());
                            }

                            if (json.contains("targetId")) {
                                accountId = std::string(json["targetId"].as_string().c_str());
                            }

                            webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
                            options.offer_to_receive_video = true;
                            options.offer_to_receive_audio = false;

                            createOfferObserver = CreateOfferObserverImpl::Create(this, peerConnection);
                            peerConnection->CreateOffer(createOfferObserver.get(), options);
                        }
                        else if (type == "answer") {
                            std::string sdp(json["sdp"].as_string().c_str());
                            processAnswer(sdp);
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
        }

        WebrtcManager::~WebrtcManager() {
            Cleanup();
        }

        // Add releaseSource implementation
        void WebrtcManager::releaseSource() {
            // Stop screen capture first (either mode)
            if (screenCapture) {
                screenCapture.reset();
            }
            if (compatScreenCapture) {
                compatScreenCapture.reset();
            }

            if (hAudioCatch) {
                hAudioCatch.reset();
            }

            if (rtcStatsCollectorHandle) {
                rtcStatsCollectorHandle = nullptr;
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

            if (audioTrack) {
                audioTrack = nullptr;
            }

            if (audioSender) {
                audioSender = nullptr;
            }

            if (dataChannel) {
                dataChannel = nullptr;
            }

            if (videoTrackSourceImpl) {
                videoTrackSourceImpl = nullptr;
            }

            if (audioDeviceModuleImpl) {
                audioDeviceModuleImpl = nullptr;
            }

            // Reset factory
            if (peerConnectionFactory) {
                peerConnectionFactory = nullptr;
            }
        }

        void WebrtcManager::Cleanup() {

            releaseSource();

            if (tcpSocket) {
                tcpSocket->closeEvent();
            }

            if (ioContextWorkPtr) {

                ioContextWorkPtr.reset();

            }

            ioContext.stop();

            if (ioContextThread.joinable()) {

                ioContextThread.join();

            }

            webrtcVideoEncoderFactory = nullptr;

            webrtcVideoDecoderFactory = nullptr;

            if (networkThread) {

                networkThread->Quit();

                networkThread.reset();

            }

            if (workerThread) {

                workerThread->Quit();

                workerThread.reset();

            }

            if (signalingThread) {

                signalingThread->Quit();

                signalingThread.reset();

            }

            if (cursorHooks) {

                cursorHooks->stopHooks();

                cursorHooks.reset();

            }

            if (hAudioCatch) {

                hAudioCatch->stopEventLoop();

                hAudioCatch.reset();

            }

            webrtc::CleanupSSL();
        }

    }

}