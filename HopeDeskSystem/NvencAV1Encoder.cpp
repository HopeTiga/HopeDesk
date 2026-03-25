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
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            createIdx(&nvencFuncs);

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
            sessionParams.device = d3dDevice.Get();
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.apiVersion = NVENCAPI_VERSION;
            nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession);

            // --- 参照 OBS init_encoder_base ---
            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID; // OBS: p1
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = 144;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 0; // 同步提取

            encodeConfig.rcParams.enableAQ = 1;

            encodeConfig.rcParams.enableMinQP = 1;
            encodeConfig.rcParams.enableMaxQP = 1;
            encodeConfig.rcParams.minQP = { 25, 25, 25 };
            encodeConfig.rcParams.maxQP = { 35, 35, 35 };

            NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
            nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID, initParams.presetGUID, initParams.tuningInfo, &presetConfig);
            encodeConfig = presetConfig.presetCfg;
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = bitrateBps * 1.2;
            encodeConfig.frameIntervalP = 1; // 无B帧以降低延迟，OBS 默认会根据 bf 调整
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

            // AV1 专属配置 (OBS 风格)
            encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
            encodeConfig.encodeCodecConfig.av1Config.enableIntraRefresh = 1;
            encodeConfig.encodeCodecConfig.av1Config.intraRefreshPeriod = 144;
            encodeConfig.encodeCodecConfig.av1Config.intraRefreshCnt = 5;
            encodeConfig.encodeCodecConfig.av1Config.idrPeriod = encodeConfig.gopLength;

            encodeConfig.encodeCodecConfig.av1Config.maxNumRefFramesInDPB = 1;

            initParams.encodeConfig = &encodeConfig;
            nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);

            // 计算缓冲深度 (参照 OBS)
            outputDelay = 0; // P1 + ULL + frameIntervalP=1 时延迟为 0
            bufCount = 4;    // 最小缓冲

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

            // 提前准备公共的送编参数
            NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
            params.inputTimeStamp = frame.render_time_ms();
            params.inputWidth = buffer->width();
            params.inputHeight = buffer->height();
            params.outputBitstream = bitstreams[idx].ptr;
            params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
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

                map.registeredResource = inputPool[idx].regPtr;
                nvencFuncs.nvEncMapInputResource(nvencSession, &map);

                params.inputBuffer = map.mappedResource;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB; 
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
            }

            dtsList.push_back(frame.render_time_ms());
            NVENCSTATUS err = nvencFuncs.nvEncEncodePicture(nvencSession, &params);

            if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
                nvencFuncs.nvEncUnmapInputResource(nvencSession, map.mappedResource);
            }
            else if (swInputBuffer) {
                nvencFuncs.nvEncDestroyInputBuffer(nvencSession, swInputBuffer);
            }

            if (err == NV_ENC_SUCCESS || err == NV_ENC_ERR_NEED_MORE_INPUT) {
                buffersQueued++;
                if (++nextBitstream == bufCount) nextBitstream = 0;
            }

            GetEncodedPacket(false);

            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencAV1Encoder::GetEncodedPacket(bool finalize) {
 
            if (!buffersQueued) return true;
            if (!finalize && buffersQueued <= outputDelay) return true;

 
            uint32_t count = finalize ? buffersQueued : 1;

            for (uint32_t i = 0; i < count; i++) {
                NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
                lock.outputBitstream = bitstreams[curBitstream].ptr;
                lock.doNotWait = false;

                if (nvencFuncs.nvEncLockBitstream(nvencSession, &lock) == NV_ENC_SUCCESS) {
       
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
        
                    image.SetEncodedData(webrtc::EncodedImageBuffer::Create((uint8_t*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes));
                    image._encodedWidth = widths;
                    image._encodedHeight = heights;
                    image.SetRtpTimestamp(static_cast<uint32_t>(lock.outputTimeStamp * 90)); 
                    image.capture_time_ms_ = lock.outputTimeStamp;
                    image._frameType = (lock.pictureType == NV_ENC_PIC_TYPE_IDR) ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;

                    int64_t dts = 0;
                    if (!dtsList.empty()) { dts = dtsList.front(); dtsList.pop_front(); }

                    webrtc::CodecSpecificInfo info;
                    info.codecType = webrtc::kVideoCodecAV1;
                    if (encodedImageCallback) encodedImageCallback->OnEncodedImage(image, &info);

                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreams[curBitstream].ptr);
                }

                if (++curBitstream == bufCount) curBitstream = 0;
                buffersQueued--;
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

                for (auto& it : inputPool) {
                    nvencFuncs.nvEncUnregisterResource(nvencSession, it.regPtr);
                }
                for (auto& bs : bitstreams) {
                    nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bs.ptr);
                }
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

            NV_ENC_RECONFIGURE_PARAMS reconfigParams = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfigParams.resetEncoder = 0; 
            reconfigParams.forceIDR = 0;

            encodeConfig.rcParams.averageBitRate = targetBitrateBps;
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(targetBitrateBps * 1.2);

            initParams.encodeConfig = &encodeConfig;
            reconfigParams.reInitEncodeParams = initParams;

            NVENCSTATUS status = nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfigParams);
            if (status != NV_ENC_SUCCESS) {

            }
        }

        webrtc::VideoEncoder::EncoderInfo NvencAV1Encoder::GetEncoderInfo() const {
            EncoderInfo info; info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCAV1";
            return info;
        }
    }
}