#include "NvencAV1Encoder.h"
#include "WebRTCD3D11TextureBuffer.h"
#include <api/video/encoded_image.h>
#include <third_party/libyuv/include/libyuv.h>
#include <dxgi.h>
#include "Utils.h"

namespace hope {
    namespace rtc {

        typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

        NvencAV1Encoder::NvencAV1Encoder() {}
        NvencAV1Encoder::~NvencAV1Encoder() { Release(); }

        int NvencAV1Encoder::InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings& settings) {
            if (!codecSettings || codecSettings->codecType != webrtc::kVideoCodecAV1) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            widths = codecSettings->width;
            heights = codecSettings->height;
            if (!InitD3D11()) return WEBRTC_VIDEO_CODEC_ERROR;
            if (!InitNvenc(widths, heights, codecSettings->startBitrate * 10000)) return WEBRTC_VIDEO_CODEC_ERROR;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencAV1Encoder::InitD3D11() {
            if (d3dDevice) return true;
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) break;
            }
            D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
            return !!d3dDevice;
        }

        bool NvencAV1Encoder::InitNvenc(int width, int height, uint32_t bitrateBps) {

            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) return false;
            auto createIdx = (PNVENCODEAPICREATEINSTANCE)GetProcAddress(nvVideoCodecHandle, "NvEncodeAPICreateInstance");

            memset(&nvencFuncs, 0, sizeof(nvencFuncs));
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            createIdx(&nvencFuncs);

  
            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams;
            memset(&sessionParams, 0, sizeof(sessionParams));
            sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            sessionParams.device = d3dDevice.Get();
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.apiVersion = NVENCAPI_VERSION;
            nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession);

            memset(&initParams, 0, sizeof(initParams));
            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 120;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 0;
            initParams.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;

            NV_ENC_PRESET_CONFIG presetConfig;
            memset(&presetConfig, 0, sizeof(presetConfig)); 
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER; 

            NVENCSTATUS presetStatus = nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID, initParams.presetGUID, initParams.tuningInfo, &presetConfig);

            if (presetStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] 获取预设参数失败！错误码: %d", presetStatus);
                return false;
            }

            // ================= 6. 复制预设并重写配置 =================
            memset(&encodeConfig, 0, sizeof(encodeConfig));
            encodeConfig = presetConfig.presetCfg;

            // ⭐ 致命补丁：结构体直接赋值可能会把正确版本号冲掉，手动强行补回！
            encodeConfig.version = NV_ENC_CONFIG_VER;
            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;

            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = bitrateBps * 1.5;
            encodeConfig.frameIntervalP = 1;
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.rcParams.enableLookahead = 0;

            encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 0;
            encodeConfig.encodeCodecConfig.av1Config.enableIntraRefresh = 0;
            encodeConfig.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 1;
            encodeConfig.encodeCodecConfig.av1Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
            encodeConfig.encodeCodecConfig.av1Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;

            initParams.encodeConfig = &encodeConfig;

            NVENCSTATUS initStatus = nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);
            if (initStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncInitializeEncoder 惨遭失败, 错误码: %d", initStatus);
                return false;
            }

            // ================= 7. 申请 Buffer 池 =================
            outputDelay = 0;
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
            }
            return true;
        }

        int NvencAV1Encoder::Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (!nvencSession) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

            auto buffer = frame.video_frame_buffer();
            uint32_t idx = nextBitstream;

            NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };

            params.inputTimeStamp = frame.rtp_timestamp();

            params.inputWidth = buffer->width();
            params.inputHeight = buffer->height();
            params.outputBitstream = bitstreams[idx].ptr;
            params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            if (frameTypes && !frameTypes->empty() && (*frameTypes)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
                params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            }

            NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
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
                    cached.km->ReleaseSync(0);
                    d3dBuffer->FreeSharedSlot();
                }
                map.registeredResource = inputPool[idx].regPtr;
                nvencFuncs.nvEncMapInputResource(nvencSession, &map);
                params.inputBuffer = map.mappedResource;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
                mappedResources[idx] = map.mappedResource;
                swInputBuffers[idx] = nullptr;
            }
            else {
                // 软件帧路径
                NV_ENC_CREATE_INPUT_BUFFER createInput = { NV_ENC_CREATE_INPUT_BUFFER_VER };
                createInput.width = buffer->width();
                createInput.height = buffer->height();
                createInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                nvencFuncs.nvEncCreateInputBuffer(nvencSession, &createInput);
                NV_ENC_INPUT_PTR swInputBuffer = createInput.inputBuffer;

                NV_ENC_LOCK_INPUT_BUFFER lockInput = { NV_ENC_LOCK_INPUT_BUFFER_VER };
                lockInput.inputBuffer = swInputBuffer;
                nvencFuncs.nvEncLockInputBuffer(nvencSession, &lockInput);

                if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
                    auto nv12 = buffer->GetNV12();
                    libyuv::CopyPlane(nv12->DataY(), nv12->StrideY(), (uint8_t*)lockInput.bufferDataPtr, lockInput.pitch, widths, heights);
                    libyuv::CopyPlane(nv12->DataUV(), nv12->StrideUV(), (uint8_t*)lockInput.bufferDataPtr + (heights * lockInput.pitch), lockInput.pitch, widths, heights / 2);
                }
                else {
                    auto i420 = buffer->ToI420();
                    libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(), i420->StrideV(),
                        (uint8_t*)lockInput.bufferDataPtr, lockInput.pitch, (uint8_t*)lockInput.bufferDataPtr + (heights * lockInput.pitch), lockInput.pitch, widths, heights);
                }
                nvencFuncs.nvEncUnlockInputBuffer(nvencSession, swInputBuffer);
                params.inputBuffer = swInputBuffer;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                mappedResources[idx] = nullptr;
                swInputBuffers[idx] = swInputBuffer;
            }

            NVENCSTATUS err = nvencFuncs.nvEncEncodePicture(nvencSession, &params);
            if (err == NV_ENC_SUCCESS) {
                // ⭐ 修复点4：dtsList 存入对应的 render_time_ms，用于后续 capture_time_ms_ 回传
                dtsList.push_back(frame.render_time_ms());
                buffersQueued++;
                if (++nextBitstream == bufCount) nextBitstream = 0;
            }
            else {
            
                LOG_INFO("err:%d", err);

            }

            

            GetEncodedPacket(false);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencAV1Encoder::GetEncodedPacket(bool finalize) {

            if (!buffersQueued) return true;

            uint32_t count = buffersQueued;

            for (uint32_t i = 0; i < count; i++) {
                NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
                lock.outputBitstream = bitstreams[curBitstream].ptr;
                lock.doNotWait = false;

                if (nvencFuncs.nvEncLockBitstream(nvencSession, &lock) == NV_ENC_SUCCESS) {
                    webrtc::EncodedImage image;
                    image.SetEncodedData(webrtc::EncodedImageBuffer::Create((uint8_t*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes));
                    image._encodedWidth = widths;
                    image._encodedHeight = heights;

                    // ⭐ 修复点5：从 NVENC 原样取回咱们刚才塞进去的 RTP 时间戳。
                    // 这样 RTP 时间戳永远是单调对齐的，彻底消除跳帧感。
                    image.SetRtpTimestamp(static_cast<uint32_t>(lock.outputTimeStamp));

                    if (!dtsList.empty()) {
                        image.capture_time_ms_ = dtsList.front();
                        dtsList.pop_front();
                    }

                    image._frameType = (lock.pictureType == NV_ENC_PIC_TYPE_IDR) ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;

                    webrtc::CodecSpecificInfo info;
                    info.codecType = webrtc::kVideoCodecAV1;
                    info.end_of_picture = true;
                    if (encodedImageCallback) encodedImageCallback->OnEncodedImage(image, &info);

                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreams[curBitstream].ptr);
                    if (mappedResources[curBitstream]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedResources[curBitstream]);
                        mappedResources[curBitstream] = nullptr;
                    }
                    if (swInputBuffers[curBitstream]) {
                        nvencFuncs.nvEncDestroyInputBuffer(nvencSession, swInputBuffers[curBitstream]);
                        swInputBuffers[curBitstream] = nullptr;
                    }

                    if (++curBitstream == bufCount) curBitstream = 0;
                    buffersQueued--;
                }
                else {
                    break;
                }
            }
            return true;
        }

        int NvencAV1Encoder::Release() {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (nvencSession) {
                NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
                params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
                nvencFuncs.nvEncEncodePicture(nvencSession, &params);
                GetEncodedPacket(true);

                for (auto& it : inputPool) nvencFuncs.nvEncUnregisterResource(nvencSession, it.regPtr);
                for (auto& bs : bitstreams) nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bs.ptr);

                resourceCache.clear();
                nvencFuncs.nvEncDestroyEncoder(nvencSession);
                nvencSession = nullptr;
            }
            if (nvVideoCodecHandle) FreeLibrary(nvVideoCodecHandle);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencAV1Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback; return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencAV1Encoder::SetRates(const RateControlParameters& parameters) {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (!nvencSession) return;

            uint32_t targetBitrateBps = parameters.bitrate.get_sum_bps();
            if (targetBitrateBps == 0) return;

            uint32_t currentBitrate = encodeConfig.rcParams.averageBitRate;
            double changeRatio = (double)targetBitrateBps / currentBitrate;

            // 2. 阈值判断：变化小于 10% 忽略
            const double MIN_CHANGE_RATIO = 0.9;   // 降低 10% 才响应
            const double MAX_CHANGE_RATIO = 1.1;     // 增加 10% 才响应

            if (changeRatio > MIN_CHANGE_RATIO && changeRatio < MAX_CHANGE_RATIO) {

                return;
            }

            // 3. 时间防抖：距离上次调整至少 500ms
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastRateChangeTime).count();

            const int64_t MIN_CHANGE_INTERVAL_MS = 500;  // 500ms 内不重复调整

            if (elapsed < MIN_CHANGE_INTERVAL_MS && changeRatio < 2.0 && changeRatio > 0.5) {

                return;
            }

            NV_ENC_RECONFIGURE_PARAMS reconfigParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfigParams.resetEncoder = 0;
            reconfigParams.forceIDR = 0;

            encodeConfig.rcParams.averageBitRate = targetBitrateBps;
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(targetBitrateBps * 1.5);  // 可以恢复 1.2 倍突发

            initParams.encodeConfig = &encodeConfig;
            reconfigParams.reInitEncodeParams = initParams;

            NVENCSTATUS status = nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfigParams);
            if (status != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] 码率重配置失败: %d", status);
                return;
            }

            // 记录调整时间
            lastRateChangeTime = now;
        }

        webrtc::VideoEncoder::EncoderInfo NvencAV1Encoder::GetEncoderInfo() const {
            EncoderInfo info; info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCAV1";
            return info;
        }
    }
}