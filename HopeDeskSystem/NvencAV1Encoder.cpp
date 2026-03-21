#include "NvencAV1Encoder.h"
#include "WebRTCD3D11TextureBuffer.h"
#include <api/video/encoded_image.h>
#include <api/video/video_frame.h>
#include <third_party/libyuv/include/libyuv.h>

#include <dxgi.h>

#include "Utils.h"

namespace hope {
    namespace rtc {

        typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

        NvencAV1Encoder::NvencAV1Encoder() {
            LOG_INFO("[NVENC] ==========================================");
            LOG_INFO("[NVENC] NvencAV1Encoder 构造函数 - 异步并发模式");
            LOG_INFO("[NVENC] ==========================================");
        }

        NvencAV1Encoder::~NvencAV1Encoder() {
            Release();
        }

        int NvencAV1Encoder::InitEncode(const webrtc::VideoCodec* codecSettings,
            const webrtc::VideoEncoder::Settings& settings) {

            if (!codecSettings) {
                LOG_INFO("[NVENC] InitEncode 失败: codecSettings 为空");
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }

            int width = codecSettings->width;
            int height = codecSettings->height;
            uint32_t targetBitrateBps = codecSettings->startBitrate * 1000;
            if (targetBitrateBps == 0) targetBitrateBps = 2000000;

            LOG_INFO("[NVENC] InitEncode 开始 - 分辨率: %dx%d, 目标码率: %u bps", width, height, targetBitrateBps);

            if (!InitD3D11()) {
                LOG_INFO("[NVENC] InitEncode 停止: D3D11 初始化未通过");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            if (!InitNvenc(width, height, targetBitrateBps)) {
                LOG_INFO("[NVENC] InitEncode 停止: NVENC 初始化未通过");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            LOG_INFO("[NVENC] InitEncode 流程执行完毕");
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencAV1Encoder::InitD3D11() {
            if (d3dDevice) return true;

            LOG_INFO("[NVENC] InitD3D11: 创建 DXGI Factory...");
            Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) return false;

            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> targetAdapter;

            for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                LOG_INFO("[NVENC] 发现适配器[%d]: %ls (VendorId=0x%04X)", i, desc.Description, desc.VendorId);
                if (desc.VendorId == 0x10DE) {
                    targetAdapter = adapter;
                    LOG_INFO("[NVENC] -> 选中 NVIDIA 硬件适配器");
                    break;
                }
            }

            if (!targetAdapter) {
                LOG_INFO("[NVENC] 错误: 未能在系统中找到 NVIDIA 显卡");
                return false;
            }

            HRESULT hr = D3D11CreateDevice(
                targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext
            );

            if (FAILED(hr)) {
                LOG_INFO("[NVENC] D3D11CreateDevice 失败, hr=0x%08X", hr);
                return false;
            }
            return true;
        }

        bool NvencAV1Encoder::InitNvenc(int width, int height, uint32_t bitrateBps) {
            LOG_INFO("[NVENC] 加载驱动 DLL: nvEncodeAPI64.dll...");
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) {
                LOG_INFO("[NVENC] 无法定位驱动文件，请检查是否安装 NVIDIA 驱动");
                return false;
            }

            PNVENCODEAPICREATEINSTANCE nvEncodeAPICreateInstance =
                (PNVENCODEAPICREATEINSTANCE)GetProcAddress(nvVideoCodecHandle, "NvEncodeAPICreateInstance");

            if (!nvEncodeAPICreateInstance) return false;

            memset(&nvencFuncs, 0, sizeof(nvencFuncs));
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            NVENCSTATUS status = nvEncodeAPICreateInstance(&nvencFuncs);
            if (status != NV_ENC_SUCCESS) {
                LOG_INFO("[NVENC] API 实例创建失败, status=%d", status);
                return false;
            }

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams;
            memset(&sessionParams, 0, sizeof(sessionParams));
            sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.device = d3dDevice.Get();
            sessionParams.apiVersion = NVENCAPI_VERSION;

            status = nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession);
            if (status != NV_ENC_SUCCESS) {
                LOG_INFO("[NVENC] 打开会话失败, status=%d", status);
                return false;
            }

            memset(&initParams, 0, sizeof(initParams));
            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;

            // 云游戏优化：P1最快预设，牺牲10%码率效率换取最低延迟
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            // 云游戏优化：超低延迟调优
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 60;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 0;
            initParams.enableEncodeAsync = 1; // 异步模式

            LOG_INFO("[NVENC] 配置编码参数 (AV1, P1, 低延迟CBR, 保留Lookahead, 无IDR)...");
            NV_ENC_PRESET_CONFIG presetConfig;
            memset(&presetConfig, 0, sizeof(presetConfig));
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

            nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID,
                initParams.presetGUID, initParams.tuningInfo, &presetConfig);

            encodeConfig = presetConfig.presetCfg;

            // 进阶低延迟 CBR 码率控制
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            // MaxBitrate 设置为平均码率的 1.15 倍，防止极速运动时直接糊成马赛克，同时避免网络拥塞
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrateBps * 1.15);

            // 云游戏优化：关闭AQ，减少编码耗时
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.aqStrength = 0;

            // 云游戏关键优化：不自动插入IDR帧，避免码率尖峰
            encodeConfig.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 1;

            // 关闭帧内刷新，交由 WebRTC 的丢包反馈机制 (PLI/FIR) 触发 forceKeyFrame，防止 WebRTC 花屏
            encodeConfig.encodeCodecConfig.av1Config.enableIntraRefresh = 0;

            // 云游戏优化：最小参考帧，降低编码延迟
            encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 2;
            encodeConfig.encodeCodecConfig.av1Config.numFwdRefs = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            encodeConfig.encodeCodecConfig.av1Config.numBwdRefs = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;

            // 告诉解码端：这是 PC 的全范围色彩
            encodeConfig.encodeCodecConfig.av1Config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            encodeConfig.encodeCodecConfig.av1Config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
            encodeConfig.encodeCodecConfig.av1Config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;

            initParams.encodeConfig = &encodeConfig;

            status = nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);
            if (status != NV_ENC_SUCCESS) {
                LOG_INFO("[NVENC] nvEncInitializeEncoder 失败, status=%d (可能硬件不支持 AV1)", status);
                return false;
            }

            LOG_INFO("[NVENC] 创建输入/输出资源池...");
            for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParams;
                memset(&bsParams, 0, sizeof(bsParams));
                bsParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                status = nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParams);
                if (status == NV_ENC_SUCCESS) {
                    bitstreamBuffers[i] = bsParams.bitstreamBuffer;
                }
                else {
                    LOG_INFO("[NVENC] 创建 Bitstream Buffer 失败, status=%d", status);
                    return false;
                }

                NV_ENC_CREATE_INPUT_BUFFER allocInput;
                memset(&allocInput, 0, sizeof(allocInput));
                allocInput.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
                allocInput.width = width;
                allocInput.height = height;
                allocInput.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
                allocInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

                status = nvencFuncs.nvEncCreateInputBuffer(nvencSession, &allocInput);
                if (status == NV_ENC_SUCCESS) {
                    sysMemBuffers[i] = allocInput.inputBuffer;
                }
                else {
                    LOG_INFO("[NVENC] 创建 Input Buffer 失败, status=%d", status);
                    return false;
                }

                // 创建并注册异步事件
                asyncEvents[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
                NV_ENC_EVENT_PARAMS eventParams;
                memset(&eventParams, 0, sizeof(eventParams));
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = asyncEvents[i];
                nvencFuncs.nvEncRegisterAsyncEvent(nvencSession, &eventParams);
            }

            // 启动异步读取线程
            isEncoding = true;
            encoderThread = std::thread(&NvencAV1Encoder::ProcessOutput, this);

            LOG_INFO("[NVENC] 初始化完全成功");
            return true;
        }

        int NvencAV1Encoder::Encode(const webrtc::VideoFrame& frame,
            const std::vector<webrtc::VideoFrameType>* frameTypes) {

            if (!nvencSession || !encodedImageCallback) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

            bool forceKeyFrame = false;
            if (frameTypes) {
                for (auto type : *frameTypes) {
                    if (type == webrtc::VideoFrameType::kVideoFrameKey) {
                        forceKeyFrame = true;
                        break;
                    }
                }
            }

            auto buffer = frame.video_frame_buffer();
            bool isNative = (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative);
            int width = buffer->width();
            int height = buffer->height();

            int bufIdx = -1;
            {
                std::unique_lock<std::mutex> lock(encodeMutex);

                // 队列满时绝不等待，主动静默丢帧保延迟
                if (pendingQueue.size() >= MAX_BUFFER_COUNT) {
                    return WEBRTC_VIDEO_CODEC_OK;
                }

                if (!isEncoding) {
                    return WEBRTC_VIDEO_CODEC_OK;
                }

                bufIdx = currentBufferIdx;
                currentBufferIdx = (currentBufferIdx + 1) % MAX_BUFFER_COUNT;
                encodeWidths[bufIdx] = width;
                encodeHeights[bufIdx] = height;

                // 延长 VideoFrameBuffer 的生命周期
                retainedBuffers[bufIdx] = buffer;
            }

            NV_ENC_PIC_PARAMS picParams;
            memset(&picParams, 0, sizeof(picParams));
            picParams.version = NV_ENC_PIC_PARAMS_VER;
            picParams.inputWidth = width;
            picParams.inputHeight = height;
            picParams.outputBitstream = bitstreamBuffers[bufIdx];
            picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            picParams.inputTimeStamp = frame.render_time_ms();
            picParams.completionEvent = asyncEvents[bufIdx];

            if (forceKeyFrame) {
                picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
                picParams.frameIdx = 0;
            }

            rtpTimestamps[bufIdx] = frame.rtp_timestamp();
            captureTimes[bufIdx] = frame.render_time_ms();

            NV_ENC_MAP_INPUT_RESOURCE mapRes;
            memset(&mapRes, 0, sizeof(mapRes));
            mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;

            NVENCSTATUS status = NV_ENC_SUCCESS;

            {
                // 保护所有的 NVENC API 调用（除了 LockBitstream）
                std::lock_guard<std::mutex> apiLock(nvencApiMutex);

                if (isNative) {
                    auto* d3dBuffer = static_cast<WebRTCD3D11TextureBuffer*>(buffer.get());
                    HANDLE sharedHandle = d3dBuffer->GetSharedHandle();
                    auto it = resourceCache.find(sharedHandle);

                    if (it == resourceCache.end()) {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
                        if (FAILED(d3dDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&sharedTex)))) {
                            retainedBuffers[bufIdx] = nullptr; // 失败时提前释放
                            return WEBRTC_VIDEO_CODEC_ERROR;
                        }

                        D3D11_TEXTURE2D_DESC desc;
                        sharedTex->GetDesc(&desc);

                        NV_ENC_BUFFER_FORMAT fmt = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) ?
                            NV_ENC_BUFFER_FORMAT_ARGB : NV_ENC_BUFFER_FORMAT_NV12;

                        NV_ENC_REGISTER_RESOURCE reg;
                        memset(&reg, 0, sizeof(reg));
                        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
                        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                        reg.resourceToRegister = sharedTex.Get();
                        reg.width = width;
                        reg.height = height;
                        reg.bufferFormat = fmt;
                        reg.bufferUsage = NV_ENC_INPUT_IMAGE;

                        if (nvencFuncs.nvEncRegisterResource(nvencSession, &reg) != NV_ENC_SUCCESS) {
                            retainedBuffers[bufIdx] = nullptr;
                            return WEBRTC_VIDEO_CODEC_ERROR;
                        }

                        resourceCache[sharedHandle] = { sharedTex, reg.registeredResource, fmt };
                        it = resourceCache.find(sharedHandle);
                    }

                    mapRes.registeredResource = it->second.registeredPtr;
                    if (nvencFuncs.nvEncMapInputResource(nvencSession, &mapRes) != NV_ENC_SUCCESS) {
                        retainedBuffers[bufIdx] = nullptr;
                        return WEBRTC_VIDEO_CODEC_ERROR;
                    }

                    picParams.inputBuffer = mapRes.mappedResource;
                    picParams.bufferFmt = it->second.format;
                    mappedInputBuffers[bufIdx] = mapRes.mappedResource;
                }
                else {
                    mappedInputBuffers[bufIdx] = nullptr;

                    NV_ENC_LOCK_INPUT_BUFFER lockInput = { NV_ENC_LOCK_INPUT_BUFFER_VER };
                    lockInput.inputBuffer = sysMemBuffers[bufIdx];

                    if (nvencFuncs.nvEncLockInputBuffer(nvencSession, &lockInput) != NV_ENC_SUCCESS) {
                        retainedBuffers[bufIdx] = nullptr;
                        return WEBRTC_VIDEO_CODEC_ERROR;
                    }

                    if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
                        auto nv12 = buffer->GetNV12();
                        if (nv12) {
                            uint8_t* dstY = (uint8_t*)lockInput.bufferDataPtr;
                            uint8_t* dstUV = dstY + (height * lockInput.pitch);

                            libyuv::CopyPlane(nv12->DataY(), nv12->StrideY(), dstY, lockInput.pitch, width, height);
                            libyuv::CopyPlane(nv12->DataUV(), nv12->StrideUV(), dstUV, lockInput.pitch, width, height / 2);
                        }
                    }
                    else {
                        auto i420 = buffer->ToI420();
                        uint8_t* dstY = (uint8_t*)lockInput.bufferDataPtr;
                        uint8_t* dstUV = dstY + (height * lockInput.pitch);

                        libyuv::I420ToNV12(
                            i420->DataY(), i420->StrideY(),
                            i420->DataU(), i420->StrideU(),
                            i420->DataV(), i420->StrideV(),
                            dstY, lockInput.pitch,
                            dstUV, lockInput.pitch,
                            width, height
                        );
                    }

                    nvencFuncs.nvEncUnlockInputBuffer(nvencSession, sysMemBuffers[bufIdx]);
                    picParams.inputBuffer = sysMemBuffers[bufIdx];
                    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                }

                // 提交给 GPU，异步进行
                status = nvencFuncs.nvEncEncodePicture(nvencSession, &picParams);

                if (status != NV_ENC_SUCCESS) {
                    if (isNative && mappedInputBuffers[bufIdx]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedInputBuffers[bufIdx]);
                        mappedInputBuffers[bufIdx] = nullptr;
                    }
                    retainedBuffers[bufIdx] = nullptr; // 失败释放
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
            }

            {
                std::unique_lock<std::mutex> lock(encodeMutex);
                pendingQueue.push(bufIdx);
                queueCond.notify_all();
            }

            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencAV1Encoder::ProcessOutput() {
            while (isEncoding) {
                int bufIdx = -1;
                {
                    std::unique_lock<std::mutex> lock(encodeMutex);
                    queueCond.wait_for(lock, std::chrono::milliseconds(1), [this] {
                        return !pendingQueue.empty() || !isEncoding;
                        });
                    if (!isEncoding && pendingQueue.empty()) break;
                    if (pendingQueue.empty()) continue;
                    bufIdx = pendingQueue.front();
                }

                // 等待 GPU 编码完成
                DWORD waitResult = WaitForSingleObject(asyncEvents[bufIdx], 100);
                if (waitResult == WAIT_TIMEOUT) {
                    continue;
                }
                else if (waitResult != WAIT_OBJECT_0) {
                    LOG_INFO("[NVENC] 异步提取 WaitForSingleObject 异常");
                    break;
                }

                NV_ENC_LOCK_BITSTREAM lockBs;
                memset(&lockBs, 0, sizeof(lockBs));
                lockBs.version = NV_ENC_LOCK_BITSTREAM_VER;
                lockBs.outputBitstream = bitstreamBuffers[bufIdx];
                lockBs.doNotWait = 0;

                // 根据 NVENC 规范，nvEncLockBitstream 是线程安全的，不需要加 nvencApiMutex
                if (nvencFuncs.nvEncLockBitstream(nvencSession, &lockBs) == NV_ENC_SUCCESS) {
                    webrtc::EncodedImage encodedImage;
                    encodedImage.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                        (uint8_t*)lockBs.bitstreamBufferPtr,
                        lockBs.bitstreamSizeInBytes));

                    encodedImage._encodedWidth = encodeWidths[bufIdx];
                    encodedImage._encodedHeight = encodeHeights[bufIdx];
                    encodedImage._frameType = (lockBs.pictureType == NV_ENC_PIC_TYPE_IDR) ?
                        webrtc::VideoFrameType::kVideoFrameKey :
                        webrtc::VideoFrameType::kVideoFrameDelta;
                    encodedImage.SetRtpTimestamp(rtpTimestamps[bufIdx]);
                    encodedImage.capture_time_ms_ = captureTimes[bufIdx];

                    webrtc::CodecSpecificInfo info;
                    info.codecType = webrtc::kVideoCodecAV1;

                    if (encodedImageCallback) {
                        encodedImageCallback->OnEncodedImage(encodedImage, &info);
                    }
                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreamBuffers[bufIdx]);
                }

                {
                    // Unmap 必须加锁，不能和 Map/Encode 并发执行
                    std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                    if (mappedInputBuffers[bufIdx]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedInputBuffers[bufIdx]);
                        mappedInputBuffers[bufIdx] = nullptr;
                    }
                }

                // 用局部变量接管智能指针的生命周期，移出临界区，彻底消除锁竞争
                webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frameToRelease;
                {
                    std::unique_lock<std::mutex> lock(encodeMutex);

                    // 转移所有权
                    frameToRelease = retainedBuffers[bufIdx];
                    retainedBuffers[bufIdx] = nullptr;

                    pendingQueue.pop();
                    queueCond.notify_all();
                }
                // 此时离开作用域，frameToRelease 析构。
                // 这触发 WebRTCD3D11TextureBuffer 释放及 hwSharedBusy 解锁，完全无锁！
            }
        }

        int NvencAV1Encoder::Release() {
            LOG_INFO("[NVENC] Release: 清理所有资源...");

            if (isEncoding) {
                isEncoding = false;
                queueCond.notify_all();
                if (encoderThread.joinable()) {
                    encoderThread.join();
                }
            }

            if (nvencSession) {
                for (auto& pair : resourceCache) {
                    nvencFuncs.nvEncUnregisterResource(nvencSession, pair.second.registeredPtr);
                }
                resourceCache.clear();

                for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
                    // 解绑注销事件
                    if (asyncEvents[i]) {
                        NV_ENC_EVENT_PARAMS eventParams;
                        memset(&eventParams, 0, sizeof(eventParams));
                        eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                        eventParams.completionEvent = asyncEvents[i];
                        nvencFuncs.nvEncUnregisterAsyncEvent(nvencSession, &eventParams);
                        CloseHandle(asyncEvents[i]);
                        asyncEvents[i] = nullptr;
                    }
                    if (bitstreamBuffers[i]) nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bitstreamBuffers[i]);
                    if (sysMemBuffers[i]) nvencFuncs.nvEncDestroyInputBuffer(nvencSession, sysMemBuffers[i]);
                }
                nvencFuncs.nvEncDestroyEncoder(nvencSession);
                nvencSession = nullptr;
            }
            if (nvVideoCodecHandle) FreeLibrary(nvVideoCodecHandle);
            d3dContext.Reset();
            d3dDevice.Reset();

            // 清理 Queue
            std::queue<int> emptyQueue;
            std::swap(pendingQueue, emptyQueue);

            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencAV1Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencAV1Encoder::SetRates(const RateControlParameters& parameters) {

        }


        webrtc::VideoEncoder::EncoderInfo NvencAV1Encoder::GetEncoderInfo() const {
            EncoderInfo info;
            info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCAV1";
            return info;
        }
    }
}