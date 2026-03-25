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
            // ⭐ 修复点1：WebRTC 传的是 kbps，乘以 1000 才是 bps。
            // 之前乘以 10000 会导致请求 2Mbps 实际给 20Mbps，带宽直接塞爆，造成视觉上的丢帧。
            if (!InitNvenc(widths, heights, codecSettings->startBitrate * 1000)) return WEBRTC_VIDEO_CODEC_ERROR;
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
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            createIdx(&nvencFuncs);

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
            sessionParams.device = d3dDevice.Get();
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.apiVersion = NVENCAPI_VERSION;
            nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession);

            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 60; // 实时流建议设为 60
            initParams.frameRateDen = 1;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 0;

            encodeConfig.rcParams.enableAQ = 1;
            encodeConfig.rcParams.enableMinQP = 1;
            encodeConfig.rcParams.enableMaxQP = 1;
            encodeConfig.rcParams.minQP = { 25, 25, 25 };
            encodeConfig.rcParams.maxQP = { 38, 38, 38 };
            encodeConfig.rcParams.enableLookahead = 0;

            NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
            nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID, initParams.presetGUID, initParams.tuningInfo, &presetConfig);
            encodeConfig = presetConfig.presetCfg;
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = bitrateBps;
            encodeConfig.frameIntervalP = 1;
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

            encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 0;
            encodeConfig.encodeCodecConfig.av1Config.enableIntraRefresh = 1;
            encodeConfig.encodeCodecConfig.av1Config.intraRefreshPeriod = 120;
            encodeConfig.encodeCodecConfig.av1Config.intraRefreshCnt = 6;
            encodeConfig.encodeCodecConfig.av1Config.idrPeriod = encodeConfig.gopLength;
            encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 1;

            initParams.encodeConfig = &encodeConfig;
            nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);

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

            GetEncodedPacket(false);

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
            if (err == NV_ENC_SUCCESS || err == NV_ENC_ERR_NEED_MORE_INPUT) {
                // ⭐ 修复点4：dtsList 存入对应的 render_time_ms，用于后续 capture_time_ms_ 回传
                dtsList.push_back(frame.render_time_ms());
                buffersQueued++;
                if (++nextBitstream == bufCount) nextBitstream = 0;
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

            // ⭐ 修复点6：Reconfigure 时也要修正码率。
            NV_ENC_RECONFIGURE_PARAMS reconfigParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfigParams.reInitEncodeParams = initParams;
            reconfigParams.reInitEncodeParams.encodeConfig->rcParams.averageBitRate = targetBitrateBps;
            reconfigParams.reInitEncodeParams.encodeConfig->rcParams.maxBitRate = targetBitrateBps;
            nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfigParams);
        }

        webrtc::VideoEncoder::EncoderInfo NvencAV1Encoder::GetEncoderInfo() const {
            EncoderInfo info; info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCAV1";
            return info;
        }
    }
}