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
            LOG_INFO("[NVENC] NvencAV1Encoder - 纯后台编码线程架构 (修复编译版)");
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
            uint32_t targetBitrateBps = codecSettings->startBitrate * 10;

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
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) {
                LOG_INFO("[NVENC] 无法定位驱动文件");
                return false;
            }

            PNVENCODEAPICREATEINSTANCE nvEncodeAPICreateInstance =
                (PNVENCODEAPICREATEINSTANCE)GetProcAddress(nvVideoCodecHandle, "NvEncodeAPICreateInstance");

            if (!nvEncodeAPICreateInstance) return false;

            memset(&nvencFuncs, 0, sizeof(nvencFuncs));
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            if (nvEncodeAPICreateInstance(&nvencFuncs) != NV_ENC_SUCCESS) return false;

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams;
            memset(&sessionParams, 0, sizeof(sessionParams));
            sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.device = d3dDevice.Get();
            sessionParams.apiVersion = NVENCAPI_VERSION;

            if (nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession) != NV_ENC_SUCCESS) return false;

            memset(&initParams, 0, sizeof(initParams));
            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 60;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 0;
            initParams.enableEncodeAsync = 1;

            NV_ENC_PRESET_CONFIG presetConfig;
            memset(&presetConfig, 0, sizeof(presetConfig));
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

            nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID,
                initParams.presetGUID, initParams.tuningInfo, &presetConfig);

            encodeConfig = presetConfig.presetCfg;
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrateBps * 1.15);
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.aqStrength = 0;
            encodeConfig.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
            encodeConfig.encodeCodecConfig.av1Config.enableIntraRefresh = 0;
            encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 2;
            encodeConfig.encodeCodecConfig.av1Config.numFwdRefs = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            encodeConfig.encodeCodecConfig.av1Config.numBwdRefs = NV_ENC_NUM_REF_FRAMES_AUTOSELECT;
            encodeConfig.encodeCodecConfig.av1Config.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            encodeConfig.encodeCodecConfig.av1Config.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
            encodeConfig.encodeCodecConfig.av1Config.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;

            initParams.encodeConfig = &encodeConfig;

            if (nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams) != NV_ENC_SUCCESS) return false;

            for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParams;
                memset(&bsParams, 0, sizeof(bsParams));
                bsParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                if (nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParams) == NV_ENC_SUCCESS) {
                    bitstreamBuffers[i] = bsParams.bitstreamBuffer;
                }

                NV_ENC_CREATE_INPUT_BUFFER allocInput;
                memset(&allocInput, 0, sizeof(allocInput));
                allocInput.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
                allocInput.width = width;
                allocInput.height = height;
                allocInput.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
                allocInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                if (nvencFuncs.nvEncCreateInputBuffer(nvencSession, &allocInput) == NV_ENC_SUCCESS) {
                    sysMemBuffers[i] = allocInput.inputBuffer;
                }

                asyncEvents[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
                NV_ENC_EVENT_PARAMS eventParams;
                memset(&eventParams, 0, sizeof(eventParams));
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = asyncEvents[i];
                nvencFuncs.nvEncRegisterAsyncEvent(nvencSession, &eventParams);
            }

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

            {
                std::unique_lock<std::mutex> lock(encodeMutex);
                // 队列满时绝不等待，主动丢帧保延迟。真正的无阻塞提交。
                if (taskQueue.size() >= MAX_BUFFER_COUNT || !isEncoding) {
                    return WEBRTC_VIDEO_CODEC_OK;
                }

                // 使用 emplace 并在内部直接调用 EncodeTask 构造函数
                taskQueue.emplace(frame, forceKeyFrame);
                queueCond.notify_all();
            }

            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencAV1Encoder::ProcessOutput() {
            int bufIdx = 0; // 内部循环使用的 NVENC 缓冲池索引

            while (isEncoding) {
                // 1. 等待并取出任务
                std::unique_lock<std::mutex> lock(encodeMutex);
                queueCond.wait(lock, [this] {
                    return !taskQueue.empty() || !isEncoding;
                    });

                if (!isEncoding && taskQueue.empty()) break;
                if (taskQueue.empty()) continue; // 虚假唤醒防范

                // 2. 利用移动构造取出数据，规避默认构造要求
                EncodeTask task = std::move(taskQueue.front());
                taskQueue.pop();

                // 3. 拿到数据立刻解锁，解放主线程
                lock.unlock();

                // ---------------- 临界区外：耗时的 GPU 通信与编码 ----------------
                auto buffer = task.frame.video_frame_buffer();
                if (!buffer) continue;

                bool isNative = (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative);
                int width = buffer->width();
                int height = buffer->height();

                NV_ENC_PIC_PARAMS picParams;
                memset(&picParams, 0, sizeof(picParams));
                picParams.version = NV_ENC_PIC_PARAMS_VER;
                picParams.inputWidth = width;
                picParams.inputHeight = height;
                picParams.outputBitstream = bitstreamBuffers[bufIdx];
                picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
                picParams.inputTimeStamp = task.frame.render_time_ms();
                picParams.completionEvent = asyncEvents[bufIdx];

                if (task.forceKeyFrame) {
                    picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
                    picParams.frameIdx = 0;
                }

                NV_ENC_MAP_INPUT_RESOURCE mapRes;
                memset(&mapRes, 0, sizeof(mapRes));
                mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;

                NVENCSTATUS status = NV_ENC_SUCCESS;
                NV_ENC_INPUT_PTR currentMappedBuffer = nullptr;

                {
                    // 保护资源交互
                    std::lock_guard<std::mutex> apiLock(nvencApiMutex);

                    if (isNative) {
                        auto* d3dBuffer = static_cast<WebRTCD3D11TextureBuffer*>(buffer.get());
                        HANDLE sharedHandle = d3dBuffer->GetSharedHandle();
                        auto it = resourceCache.find(sharedHandle);

                        if (it == resourceCache.end()) {
                            Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
                            if (SUCCEEDED(d3dDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&sharedTex)))) {
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

                                if (nvencFuncs.nvEncRegisterResource(nvencSession, &reg) == NV_ENC_SUCCESS) {
                                    resourceCache[sharedHandle] = { sharedTex, reg.registeredResource, fmt };
                                    it = resourceCache.find(sharedHandle);
                                }
                            }
                        }

                        if (it != resourceCache.end()) {
                            mapRes.registeredResource = it->second.registeredPtr;
                            if (nvencFuncs.nvEncMapInputResource(nvencSession, &mapRes) == NV_ENC_SUCCESS) {
                                picParams.inputBuffer = mapRes.mappedResource;
                                picParams.bufferFmt = it->second.format;
                                currentMappedBuffer = mapRes.mappedResource;
                            }
                        }
                    }
                    else {
                        NV_ENC_LOCK_INPUT_BUFFER lockInput = { NV_ENC_LOCK_INPUT_BUFFER_VER };
                        lockInput.inputBuffer = sysMemBuffers[bufIdx];

                        if (nvencFuncs.nvEncLockInputBuffer(nvencSession, &lockInput) == NV_ENC_SUCCESS) {
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
                                    i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(), i420->StrideV(),
                                    dstY, lockInput.pitch, dstUV, lockInput.pitch, width, height
                                );
                            }
                            nvencFuncs.nvEncUnlockInputBuffer(nvencSession, sysMemBuffers[bufIdx]);
                            picParams.inputBuffer = sysMemBuffers[bufIdx];
                            picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                        }
                    }

                    if (picParams.inputBuffer) {
                        status = nvencFuncs.nvEncEncodePicture(nvencSession, &picParams);
                    }
                    else {
                        status = NV_ENC_ERR_INVALID_PARAM;
                    }
                }

                if (status == NV_ENC_SUCCESS) {
                    DWORD waitResult = WaitForSingleObject(asyncEvents[bufIdx], 500);
                    if (waitResult == WAIT_OBJECT_0) {
                        NV_ENC_LOCK_BITSTREAM lockBs;
                        memset(&lockBs, 0, sizeof(lockBs));
                        lockBs.version = NV_ENC_LOCK_BITSTREAM_VER;
                        lockBs.outputBitstream = bitstreamBuffers[bufIdx];
                        lockBs.doNotWait = 0;

                        if (nvencFuncs.nvEncLockBitstream(nvencSession, &lockBs) == NV_ENC_SUCCESS) {
                            webrtc::EncodedImage encodedImage;
                            encodedImage.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                                (uint8_t*)lockBs.bitstreamBufferPtr, lockBs.bitstreamSizeInBytes));

                            encodedImage._encodedWidth = width;
                            encodedImage._encodedHeight = height;
                            encodedImage._frameType = (lockBs.pictureType == NV_ENC_PIC_TYPE_IDR) ?
                                webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;

                            // 从本循环独占的 task 对象中取值，绝对线程安全
                            encodedImage.SetRtpTimestamp(task.frame.rtp_timestamp());
                            encodedImage.capture_time_ms_ = task.frame.render_time_ms();

                            webrtc::CodecSpecificInfo info;
                            info.codecType = webrtc::kVideoCodecAV1;

                            if (encodedImageCallback) {
                                encodedImageCallback->OnEncodedImage(encodedImage, &info);
                            }
                            nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreamBuffers[bufIdx]);
                        }
                    }
                }

                if (currentMappedBuffer) {
                    std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                    nvencFuncs.nvEncUnmapInputResource(nvencSession, currentMappedBuffer);
                }

                // 推进 NVENC 缓冲池索引
                bufIdx = (bufIdx + 1) % MAX_BUFFER_COUNT;

                // 循环结束时，局部变量 task 自动析构。
                // 这意味着 webrtc::VideoFrame 生命终结，自动释放底层资源 (解锁 D3D11 Texture 等)
            }
        }

        int NvencAV1Encoder::Release() {
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

            // 利用空队列进行交换，强制析构所有遗留的 EncodeTask，防止内存泄露
            std::queue<EncodeTask> emptyQueue;
            std::swap(taskQueue, emptyQueue);

            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencAV1Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencAV1Encoder::SetRates(const RateControlParameters& parameters) {}

        webrtc::VideoEncoder::EncoderInfo NvencAV1Encoder::GetEncoderInfo() const {
            EncoderInfo info;
            info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCAV1";
            return info;
        }
    }
}