#include "NvencH265Encoder.h"
#include "WebRTCD3D11TextureBuffer.h"
#include <api/video/encoded_image.h>
#include <api/video/video_frame.h>
#include <third_party/libyuv/include/libyuv.h>

#include <dxgi.h>

#include "Utils.h"

namespace hope {
    namespace rtc {

        typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

        NvencH265Encoder::NvencH265Encoder() {
            LOG_INFO("[NVENC] NvencH265Encoder Constructor - Async HEVC");
        }

        NvencH265Encoder::~NvencH265Encoder() {
            Release();
        }

        int NvencH265Encoder::InitEncode(const webrtc::VideoCodec* codecSettings,
            const webrtc::VideoEncoder::Settings& settings) {

            if (!codecSettings) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

            int width = codecSettings->width;
            int height = codecSettings->height;
            uint32_t targetBitrateBps = codecSettings->startBitrate * 1000;
            if (targetBitrateBps == 0) targetBitrateBps = 2000000;

            if (!InitD3D11() || !InitNvenc(width, height, targetBitrateBps)) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencH265Encoder::InitD3D11() {
            if (d3dDevice) return true;

            Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) return false;

            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> targetAdapter;

            for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) { // NVIDIA
                    targetAdapter = adapter;
                    break;
                }
            }

            if (!targetAdapter) return false;

            HRESULT hr = D3D11CreateDevice(
                targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext
            );

            return SUCCEEDED(hr);
        }

        bool NvencH265Encoder::InitNvenc(int width, int height, uint32_t bitrateBps) {
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) return false;

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
            initParams.encodeGUID = NV_ENC_CODEC_HEVC_GUID; // ʹ�� H.265 (HEVC) GUID
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
            encodeConfig.rcParams.vbvBufferSize = bitrateBps / (60 / 2);
            encodeConfig.rcParams.vbvInitialDelay = encodeConfig.rcParams.vbvBufferSize;
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.enableLookahead = 1;
            encodeConfig.rcParams.lookaheadDepth = 8;

            // HEVC ר������
            encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1; // �滻�� AV1 �� repeatSeqHdr
            encodeConfig.encodeCodecConfig.hevcConfig.enableIntraRefresh = 0;
            encodeConfig.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 2;

            initParams.encodeConfig = &encodeConfig;

            if (nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams) != NV_ENC_SUCCESS) return false;

            for (int i = 0; i < MaxBufferCount; i++) {
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParams;
                memset(&bsParams, 0, sizeof(bsParams));
                bsParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                if (nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParams) == NV_ENC_SUCCESS) {
                    bitstreamBuffers[i] = bsParams.bitstreamBuffer;
                }
                else return false;

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
                else return false;

                asyncEvents[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
                NV_ENC_EVENT_PARAMS eventParams;
                memset(&eventParams, 0, sizeof(eventParams));
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = asyncEvents[i];
                nvencFuncs.nvEncRegisterAsyncEvent(nvencSession, &eventParams);
            }

            isEncoding = true;
            encoderThread = std::thread(&NvencH265Encoder::ProcessOutput, this);

            return true;
        }

        int NvencH265Encoder::Encode(const webrtc::VideoFrame& frame,
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
                if (pendingQueue.size() >= MaxBufferCount) return WEBRTC_VIDEO_CODEC_OK;
                if (!isEncoding) return WEBRTC_VIDEO_CODEC_OK;

                bufIdx = currentBufferIdx;
                currentBufferIdx = (currentBufferIdx + 1) % MaxBufferCount;
                encodeWidths[bufIdx] = width;
                encodeHeights[bufIdx] = height;
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
                std::lock_guard<std::mutex> apiLock(nvencApiMutex);

                if (isNative) {
                    auto* d3dBuffer = static_cast<WebRTCD3D11TextureBuffer*>(buffer.get());
                    HANDLE sharedHandle = d3dBuffer->GetSharedHandle();
                    auto it = resourceCache.find(sharedHandle);

                    if (it == resourceCache.end()) {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
                        if (FAILED(d3dDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&sharedTex)))) {
                            retainedBuffers[bufIdx] = nullptr;
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

                status = nvencFuncs.nvEncEncodePicture(nvencSession, &picParams);

                if (status != NV_ENC_SUCCESS) {
                    if (isNative && mappedInputBuffers[bufIdx]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedInputBuffers[bufIdx]);
                        mappedInputBuffers[bufIdx] = nullptr;
                    }
                    retainedBuffers[bufIdx] = nullptr;
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

        void NvencH265Encoder::ProcessOutput() {
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

                DWORD waitResult = WaitForSingleObject(asyncEvents[bufIdx], 100);
                if (waitResult == WAIT_TIMEOUT) {
                    continue;
                }
                else if (waitResult != WAIT_OBJECT_0) {
                    break;
                }

                NV_ENC_LOCK_BITSTREAM lockBs;
                memset(&lockBs, 0, sizeof(lockBs));
                lockBs.version = NV_ENC_LOCK_BITSTREAM_VER;
                lockBs.outputBitstream = bitstreamBuffers[bufIdx];
                lockBs.doNotWait = 0;

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
                    info.codecType = webrtc::kVideoCodecH265; // ����Ϊ H265

                    if (encodedImageCallback) {
                        encodedImageCallback->OnEncodedImage(encodedImage, &info);
                    }
                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreamBuffers[bufIdx]);
                }

                {
                    std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                    if (mappedInputBuffers[bufIdx]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedInputBuffers[bufIdx]);
                        mappedInputBuffers[bufIdx] = nullptr;
                    }
                }

                webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frameToRelease;
                {
                    std::unique_lock<std::mutex> lock(encodeMutex);
                    frameToRelease = retainedBuffers[bufIdx];
                    retainedBuffers[bufIdx] = nullptr;
                    pendingQueue.pop();
                    queueCond.notify_all();
                }
            }
        }

        int NvencH265Encoder::Release() {
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

                for (int i = 0; i < MaxBufferCount; i++) {
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

            std::queue<int> emptyQueue;
            std::swap(pendingQueue, emptyQueue);

            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencH265Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencH265Encoder::SetRates(const RateControlParameters& parameters) {
            uint32_t bitrate = parameters.bitrate.get_sum_bps();
            if (!nvencSession || bitrate == 0) return;

            uint32_t currentBitrate = encodeConfig.rcParams.averageBitRate;
            if (abs((long long)bitrate - (long long)currentBitrate) < (currentBitrate * 0.05)) {
                return;
            }

            encodeConfig.rcParams.averageBitRate = bitrate;
            encodeConfig.rcParams.maxBitRate = bitrate;

            NV_ENC_RECONFIGURE_PARAMS reconfig;
            memset(&reconfig, 0, sizeof(reconfig));
            reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
            reconfig.reInitEncodeParams = initParams;
            reconfig.reInitEncodeParams.encodeConfig = &encodeConfig;

            std::lock_guard<std::mutex> apiLock(nvencApiMutex);
            nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfig);
        }

        webrtc::VideoEncoder::EncoderInfo NvencH265Encoder::GetEncoderInfo() const {
            EncoderInfo info;
            info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NvencH265Async";
            return info;
        }
    }
}