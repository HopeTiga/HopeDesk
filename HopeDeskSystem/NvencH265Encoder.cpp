#include "NvencH265Encoder.h"
#include "WebRTCD3D11TextureBuffer.h"
#include <api/video/encoded_image.h>
#include <third_party/libyuv/include/libyuv.h>
#include <dxgi.h>
#include "Utils.h"

namespace hope {
    namespace rtc {

        typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

        NvencH265Encoder::NvencH265Encoder() {}

        NvencH265Encoder::~NvencH265Encoder() {
            Release();
        }

        int NvencH265Encoder::InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings& settings) {
            if (!codecSettings || codecSettings->codecType != webrtc::kVideoCodecH265) {
                LOG_ERROR("[NVENC] Invalid codec settings or not H265.");
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            widths = codecSettings->width;
            heights = codecSettings->height;

            if (!InitD3D11()) {
                LOG_ERROR("[NVENC] InitD3D11 failed.");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            if (!InitNvenc(widths, heights, codecSettings->startBitrate * 1000)) {
                LOG_ERROR("[NVENC] InitNvenc failed.");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            LOG_INFO("[NVENC] InitEncode success. Width: %d, Height: %d", widths, heights);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencH265Encoder::InitD3D11() {
            if (d3dDevice) return true;
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) break;
            }
            HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
            if (FAILED(hr)) {
                LOG_ERROR("[NVENC] D3D11CreateDevice failed: 0x%X", hr);
            }
            return !!d3dDevice;
        }

        bool NvencH265Encoder::InitNvenc(int width, int height, uint32_t bitrateBps) {
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) {
                LOG_ERROR("[NVENC] Failed to load nvEncodeAPI64.dll");
                return false;
            }
            auto createIdx = (PNVENCODEAPICREATEINSTANCE)GetProcAddress(nvVideoCodecHandle, "NvEncodeAPICreateInstance");
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            createIdx(&nvencFuncs);

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
            sessionParams.device = d3dDevice.Get();
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.apiVersion = NVENCAPI_VERSION;
            if (nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession) != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncOpenEncodeSessionEx failed.");
                return false;
            }

            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 60;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 1;

            NV_ENC_PRESET_CONFIG presetConfig;
            memset(&presetConfig, 0, sizeof(presetConfig));
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
            NVENCSTATUS presetStatus = nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID, initParams.presetGUID, initParams.tuningInfo, &presetConfig);

            if (presetStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] 获取预设参数失败！错误码: %d", presetStatus);
                return false;
            }
            encodeConfig = presetConfig.presetCfg;

            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = bitrateBps;
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.enableMinQP = 1;
            encodeConfig.rcParams.enableMaxQP = 1;
            encodeConfig.rcParams.minQP = { 25, 25, 25 };
            encodeConfig.rcParams.maxQP = { 38, 38, 38 };
            encodeConfig.rcParams.enableLookahead = 0;
            encodeConfig.frameIntervalP = 1;
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

            encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
            encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.encodeCodecConfig.hevcConfig.enableIntraRefresh = 0;
            encodeConfig.encodeCodecConfig.hevcConfig.intraRefreshPeriod = 60;
            encodeConfig.encodeCodecConfig.hevcConfig.intraRefreshCnt = 10;

            initParams.encodeConfig = &encodeConfig;

            NVENCSTATUS initStatus = nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);
            if (initStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncInitializeEncoder failed! ErrorCode: %d, Width: %d, Height: %d", initStatus, width, height);
                return false;
            }

            bufCount = 8;
            mappedResources.resize(bufCount, nullptr);
            swInputBuffers.resize(bufCount, nullptr);

            for (uint32_t i = 0; i < bufCount; i++) {
                NvBitstream bs;
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParam = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
                nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParam);
                bs.ptr = bsParam.bitstreamBuffer;
                bitstreams.push_back(bs);

                NvInputTexture it;
                D3D11_TEXTURE2D_DESC desc = { (UINT)width, (UINT)height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1,0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
                d3dDevice->CreateTexture2D(&desc, nullptr, &it.tex);

                NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
                reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                reg.resourceToRegister = it.tex.Get();
                reg.width = width; reg.height = height;
                reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
                nvencFuncs.nvEncRegisterResource(nvencSession, &reg);
                it.regPtr = reg.registeredResource;
                inputPool.push_back(it);

                HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
                eventParams.completionEvent = evt;
                nvencFuncs.nvEncRegisterAsyncEvent(nvencSession, &eventParams);
                encodeEvents.push_back(evt);
            }

            isRunning = true;
            outputThread = std::thread(&NvencH265Encoder::ProcessOutputThread, this);

            return true;
        }

        int NvencH265Encoder::Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) {
            {
                std::unique_lock<std::mutex> qLock(queueMutex);
                queueCond.wait(qLock, [this] { return buffersQueued < bufCount; });
            }

            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (!nvencSession) {
                return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            }

            auto buffer = frame.video_frame_buffer();
            uint32_t idx = nextBitstream;

            NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
            params.inputTimeStamp = frame.render_time_ms();
            params.inputWidth = buffer->width();
            params.inputHeight = buffer->height();
            params.outputBitstream = bitstreams[idx].ptr;
            params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            params.completionEvent = encodeEvents[idx];

            if (frameTypes && !frameTypes->empty() && (*frameTypes)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
                params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            }

            NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
            NV_ENC_INPUT_PTR swInputBuffer = nullptr;

            if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
                auto* d3dBuffer = static_cast<WebRTCD3D11TextureBuffer*>(buffer.get());
                HANDLE h = d3dBuffer->GetSharedHandle();

                auto& cached = resourceCache[h];
                if (!cached.tex) {
                    d3dDevice->OpenSharedResource(h, IID_PPV_ARGS(&cached.tex));
                    cached.tex.As(&cached.km);
                }

                if (cached.km && cached.km->AcquireSync(0, INFINITE) == S_OK) {
                    d3dContext->CopyResource(inputPool[idx].tex.Get(), cached.tex.Get());
                    d3dContext->Flush();
                    cached.km->ReleaseSync(0);
                    d3dBuffer->FreeSharedSlot();
                }
                else {
                    LOG_ERROR("[NVENC] D3D AcquireSync failed.");
                }

                map.registeredResource = inputPool[idx].regPtr;
                nvencFuncs.nvEncMapInputResource(nvencSession, &map);

                params.inputBuffer = map.mappedResource;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;

                mappedResources[idx] = map.mappedResource;
                swInputBuffers[idx] = nullptr;
            }
            else {
                int width = buffer->width();
                int height = buffer->height();

                NV_ENC_CREATE_INPUT_BUFFER createInput = { NV_ENC_CREATE_INPUT_BUFFER_VER };
                createInput.width = width;
                createInput.height = height;
                createInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                nvencFuncs.nvEncCreateInputBuffer(nvencSession, &createInput);
                swInputBuffer = createInput.inputBuffer;

                NV_ENC_LOCK_INPUT_BUFFER lockInput = { NV_ENC_LOCK_INPUT_BUFFER_VER };
                lockInput.inputBuffer = swInputBuffer;
                nvencFuncs.nvEncLockInputBuffer(nvencSession, &lockInput);

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
                    if (i420) {
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
                }

                nvencFuncs.nvEncUnlockInputBuffer(nvencSession, swInputBuffer);

                params.inputBuffer = swInputBuffer;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

                mappedResources[idx] = nullptr;
                swInputBuffers[idx] = swInputBuffer;
            }

            dtsList.push_back(frame.render_time_ms());

            NVENCSTATUS err = nvencFuncs.nvEncEncodePicture(nvencSession, &params);

            if (err == NV_ENC_SUCCESS || err == NV_ENC_ERR_NEED_MORE_INPUT) {
                {
                    std::lock_guard<std::mutex> qLock(queueMutex);
                    buffersQueued++;
                    if (++nextBitstream == bufCount) nextBitstream = 0;
                }
                queueCond.notify_one();
            }
            else {
                if (mappedResources[idx]) {
                    nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedResources[idx]);
                    mappedResources[idx] = nullptr;
                }
                if (swInputBuffers[idx]) {
                    nvencFuncs.nvEncDestroyInputBuffer(nvencSession, swInputBuffers[idx]);
                    swInputBuffers[idx] = nullptr;
                }
            }

            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencH265Encoder::ProcessOutputThread() {
            while (isRunning) {
                uint32_t processIdx = 0;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    queueCond.wait(lock, [this] { return buffersQueued > 0 || !isRunning; });
                    if (!isRunning && buffersQueued == 0) break;
                    processIdx = curBitstream;
                }

                DWORD waitRes = WaitForSingleObject(encodeEvents[processIdx], INFINITE);
                if (waitRes != WAIT_OBJECT_0) continue;

                std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                if (!nvencSession) continue;

                NV_ENC_LOCK_BITSTREAM bitstreamLock = { NV_ENC_LOCK_BITSTREAM_VER };
                bitstreamLock.outputBitstream = bitstreams[processIdx].ptr;
                bitstreamLock.doNotWait = false;

                NVENCSTATUS lockStatus = nvencFuncs.nvEncLockBitstream(nvencSession, &bitstreamLock);
                if (lockStatus == NV_ENC_SUCCESS) {
                    if (firstPacket) {
                        uint8_t header_buf[1024]; uint32_t header_size = 0;
                        NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = { NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER };
                        payload.spsppsBuffer = header_buf;
                        payload.inBufferSize = 1024;
                        payload.outSPSPPSPayloadSize = &header_size;
                        nvencFuncs.nvEncGetSequenceParams(nvencSession, &payload);
                        header.assign(header_buf, header_buf + header_size);
                        firstPacket = false;
                    }

                    webrtc::EncodedImage image;
                    image.SetEncodedData(webrtc::EncodedImageBuffer::Create((uint8_t*)bitstreamLock.bitstreamBufferPtr, bitstreamLock.bitstreamSizeInBytes));
                    image._encodedWidth = widths;
                    image._encodedHeight = heights;
                    image.SetRtpTimestamp(static_cast<uint32_t>(bitstreamLock.outputTimeStamp * 90));
                    image.capture_time_ms_ = bitstreamLock.outputTimeStamp;
                    image._frameType = (bitstreamLock.pictureType == NV_ENC_PIC_TYPE_IDR) ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;

                    int64_t dts = 0;
                    if (!dtsList.empty()) { dts = dtsList.front(); dtsList.pop_front(); }

                    webrtc::CodecSpecificInfo info;
                    info.codecType = webrtc::kVideoCodecH265;
                    if (encodedImageCallback) {
                        encodedImageCallback->OnEncodedImage(image, &info);
                    }
                    else {
                        LOG_ERROR("[NVENC] encodedImageCallback is NULL!");
                    }

                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreams[processIdx].ptr);
                }
                else {
                    LOG_ERROR("[NVENC] nvEncLockBitstream failed: %d", lockStatus);
                }

                if (mappedResources[processIdx]) {
                    nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedResources[processIdx]);
                    mappedResources[processIdx] = nullptr;
                }
                if (swInputBuffers[processIdx]) {
                    nvencFuncs.nvEncDestroyInputBuffer(nvencSession, swInputBuffers[processIdx]);
                    swInputBuffers[processIdx] = nullptr;
                }

                {
                    std::lock_guard<std::mutex> qLock(queueMutex);
                    if (++curBitstream == bufCount) curBitstream = 0;
                    buffersQueued--;
                }
                queueCond.notify_all();
            }
        }

        int NvencH265Encoder::Release() {
            if (nvencSession && isRunning) {
                {
                    std::unique_lock<std::mutex> qLock(queueMutex);
                    queueCond.wait(qLock, [this] { return buffersQueued < bufCount; });
                }

                {
                    std::lock_guard<std::mutex> lock(nvencApiMutex);
                    NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
                    params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
                    if (!encodeEvents.empty()) {
                        params.completionEvent = encodeEvents[nextBitstream];
                    }
                    nvencFuncs.nvEncEncodePicture(nvencSession, &params);

                    {
                        std::lock_guard<std::mutex> qLock(queueMutex);
                        buffersQueued++;
                        if (++nextBitstream == bufCount) nextBitstream = 0;
                    }
                    queueCond.notify_one();
                }

                while (true) {
                    {
                        std::lock_guard<std::mutex> qLock(queueMutex);
                        if (buffersQueued == 0) break;
                    }
                }
            }

            if (isRunning) {
                isRunning = false;
                queueCond.notify_all();
                if (outputThread.joinable()) {
                    outputThread.join();
                }
            }

            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (nvencSession) {
                for (uint32_t i = 0; i < encodeEvents.size(); i++) {
                    if (encodeEvents[i]) {
                        NV_ENC_EVENT_PARAMS eventParams = { NV_ENC_EVENT_PARAMS_VER };
                        eventParams.completionEvent = encodeEvents[i];
                        nvencFuncs.nvEncUnregisterAsyncEvent(nvencSession, &eventParams);
                        CloseHandle(encodeEvents[i]);
                    }
                }
                encodeEvents.clear();

                for (auto& it : inputPool) {
                    if (it.regPtr) nvencFuncs.nvEncUnregisterResource(nvencSession, it.regPtr);
                }
                for (auto& bs : bitstreams) {
                    if (bs.ptr) nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bs.ptr);
                }

                inputPool.clear();
                bitstreams.clear();
                resourceCache.clear();

                nvencFuncs.nvEncDestroyEncoder(nvencSession);
                nvencSession = nullptr;
            }
            if (nvVideoCodecHandle) {
                FreeLibrary(nvVideoCodecHandle);
                nvVideoCodecHandle = nullptr;
            }

            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencH265Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencH265Encoder::SetRates(const RateControlParameters& parameters) {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (!nvencSession) return;

            uint32_t targetBitrateBps = parameters.bitrate.get_sum_bps();
            if (targetBitrateBps == 0) return;

            uint32_t currentBitrate = encodeConfig.rcParams.averageBitRate;
            double changeRatio = (double)targetBitrateBps / currentBitrate;

            const double MIN_CHANGE_RATIO = 0.9;
            const double MAX_CHANGE_RATIO = 1.1;

            if (changeRatio > MIN_CHANGE_RATIO && changeRatio < MAX_CHANGE_RATIO) {
                return;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRateChangeTime).count();
            const int64_t MIN_CHANGE_INTERVAL_MS = 500;

            if (elapsed < MIN_CHANGE_INTERVAL_MS && changeRatio < 2.0 && changeRatio > 0.5) {
                return;
            }

            NV_ENC_RECONFIGURE_PARAMS reconfigParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfigParams.resetEncoder = 0;
            reconfigParams.forceIDR = 0;

            encodeConfig.rcParams.averageBitRate = targetBitrateBps;
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(targetBitrateBps * 1.2);

            initParams.encodeConfig = &encodeConfig;
            reconfigParams.reInitEncodeParams = initParams;

            NVENCSTATUS status = nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfigParams);
            if (status != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncReconfigureEncoder failed: %d", status);
                return;
            }
            lastRateChangeTime = now;
        }

        webrtc::VideoEncoder::EncoderInfo NvencH265Encoder::GetEncoderInfo() const {
            EncoderInfo info; info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCH265";
            return info;
        }
    }
}