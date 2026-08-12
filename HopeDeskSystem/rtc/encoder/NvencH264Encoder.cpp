#include "NvencH264Encoder.h"
#include "../buffer/WebrtcD3D11TextureBuffer.h"
#include <api/video/encoded_image.h>
#include <third_party/libyuv/libyuv.h>
#include <dxgi.h>
#include <d3d11_1.h>
#include "../../utils/Utils.h"

namespace hope {
    namespace rtc {

        typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

        // keyed-mutex 有界等待：超时=本帧跳过（背压），仅非超时失败才算 handle 死亡。
        // 参考 Sunshine bounded_consumer_acquire_timeout_ms。100ms 取中值防误跳。
        constexpr DWORD kVddAcquireTimeoutMs = 100;

        NvencH264Encoder::NvencH264Encoder() {}

        NvencH264Encoder::~NvencH264Encoder() {
            Release();
        }

        int NvencH264Encoder::InitEncode(const webrtc::VideoCodec* codecSettings,
            const webrtc::VideoEncoder::Settings& settings) {
            if (!codecSettings || codecSettings->codecType != webrtc::kVideoCodecH264) {
                LOG_ERROR("[NVENC] Invalid codec settings or not H264.");
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            widths = codecSettings->width;
            heights = codecSettings->height;

            if (!InitD3D11()) {
                LOG_ERROR("[NVENC] InitD3D11 failed.");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            if (!InitNvenc(widths, heights, codecSettings->startBitrate * 10000, codecSettings->maxFramerate)) {
                LOG_ERROR("[NVENC] InitNvenc failed.");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            LOG_INFO("[NVENC] InitEncode success. Width: %d, Height: %d, bufCount: %u", widths, heights, bufCount);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencH264Encoder::InitD3D11() {
            if (d3dDevice) return true;

            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE) break;
            }
            HRESULT hr = D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                &d3dDevice, nullptr, &d3dContext);
            if (FAILED(hr)) {
                LOG_ERROR("[NVENC] D3D11CreateDevice failed: 0x%X", hr);
            }
            return !!d3dDevice;
        }

        bool NvencH264Encoder::InitNvenc(int width, int height, uint32_t bitrateBps, uint32_t maxFramerate) {
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) {
                LOG_ERROR("[NVENC] Failed to load nvEncodeAPI64.dll");
                return false;
            }
            auto createIdx = (PNVENCODEAPICREATEINSTANCE)GetProcAddress(
                nvVideoCodecHandle, "NvEncodeAPICreateInstance");
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            createIdx(&nvencFuncs);

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {
                NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER
            };
            sessionParams.device = d3dDevice.Get();
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.apiVersion = NVENCAPI_VERSION;
            if (nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession) !=
                NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncOpenEncodeSessionEx failed.");
                return false;
            }

            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            initParams.encodeWidth = width;
            initParams.encodeHeight = height;
            initParams.darWidth = width;
            initParams.darHeight = height;
            initParams.frameRateNum = maxFramerate ? maxFramerate : 60;
            initParams.frameRateDen = 1;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 0;  // 同步模式

            NV_ENC_PRESET_CONFIG presetConfig;
            memset(&presetConfig, 0, sizeof(presetConfig));
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
            NVENCSTATUS presetStatus = nvencFuncs.nvEncGetEncodePresetConfigEx(
                nvencSession, initParams.encodeGUID, initParams.presetGUID,
                initParams.tuningInfo, &presetConfig);
            if (presetStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] Obtain preset config failed: %d", presetStatus);
                return false;
            }
            encodeConfig = presetConfig.presetCfg;

            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = bitrateBps * 2;
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.enableLookahead = 0;
            encodeConfig.frameIntervalP = 1;
            encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;

            encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            encodeConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;

            initParams.encodeConfig = &encodeConfig;

            NVENCSTATUS initStatus = nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams);
            if (initStatus != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] nvEncInitializeEncoder failed! ErrorCode: %d", initStatus);
                return false;
            }

            bufCount = 8;
            mappedResources.resize(bufCount, nullptr);
            swInputBuffers.resize(bufCount, nullptr);
            pendingInputs.resize(bufCount);

            if (!InitVideoProcessor(width, height)) {
                LOG_ERROR("[NVENC-H264] VideoProcessor 初始化失败，无法走 NV12 输入路径");
                return false;
            }

            for (uint32_t i = 0; i < bufCount; i++) {
                NvBitstream bs;
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParam = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
                nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParam);
                bs.ptr = bsParam.bitstreamBuffer;
                bitstreams.push_back(bs);

                // NV12 池槽：VP 输出目标 + NVENC 输入
                NvInputTexture it;
                D3D11_TEXTURE2D_DESC desc = {
                    (UINT)width, (UINT)height, 1, 1,
                    DXGI_FORMAT_NV12, {1, 0},
                    D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0
                };
                d3dDevice->CreateTexture2D(&desc, nullptr, &it.tex);

                NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
                reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
                reg.resourceToRegister = it.tex.Get();
                reg.width = width; reg.height = height;
                reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
                nvencFuncs.nvEncRegisterResource(nvencSession, &reg);
                it.regPtr = reg.registeredResource;

                // 每帧 VideoProcessorBlt 把 BGRA 转进这个池槽（VP 输出视图，池槽维度）
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
                ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                ovDesc.Texture2D.MipSlice = 0;
                HRESULT ovr = videoDevice->CreateVideoProcessorOutputView(it.tex.Get(), vpEnumerator.Get(), &ovDesc, &it.vpOutputView);
                if (FAILED(ovr) || !it.vpOutputView) {
                    LOG_ERROR("[NVENC-H264] CreateVideoProcessorOutputView(slot %u) 失败 hr=0x%08X", i, (unsigned)ovr);
                    return false;
                }
                inputPool.push_back(it);

                NV_ENC_CREATE_INPUT_BUFFER createInput = { NV_ENC_CREATE_INPUT_BUFFER_VER };
                createInput.width = width;
                createInput.height = height;
                createInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                nvencFuncs.nvEncCreateInputBuffer(nvencSession, &createInput);
                swInputBuffers[i] = createInput.inputBuffer;
            }

            nextBitstream = 0;
            curBitstream = 0;
            buffersQueued = 0;

            LOG_INFO("[NVENC] InitNvenc success. bufCount=%u, async=0 (sync mode)", bufCount);
            return true;
        }

        bool NvencH264Encoder::InitVideoProcessor(int width, int height) {
            if (videoProcessor) return true;

            HRESULT hr = d3dDevice.As(&videoDevice);
            if (FAILED(hr) || !videoDevice) {
                LOG_ERROR("[NVENC-H264] ID3D11VideoDevice 不可用 hr=0x%08X", (unsigned)hr);
                return false;
            }
            hr = d3dContext.As(&videoContext);
            if (FAILED(hr) || !videoContext) {
                LOG_ERROR("[NVENC-H264] ID3D11VideoContext 不可用 hr=0x%08X", (unsigned)hr);
                return false;
            }

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
            desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            desc.InputWidth = (UINT)width;
            desc.InputHeight = (UINT)height;
            desc.OutputWidth = (UINT)width;
            desc.OutputHeight = (UINT)height;
            desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            hr = videoDevice->CreateVideoProcessorEnumerator(&desc, &vpEnumerator);
            if (FAILED(hr) || !vpEnumerator) {
                LOG_ERROR("[NVENC-H264] CreateVideoProcessorEnumerator 失败 hr=0x%08X", (unsigned)hr);
                return false;
            }
            hr = videoDevice->CreateVideoProcessor(vpEnumerator.Get(), 0, &videoProcessor);
            if (FAILED(hr) || !videoProcessor) {
                LOG_ERROR("[NVENC-H264] CreateVideoProcessor 失败 hr=0x%08X", (unsigned)hr);
                return false;
            }

            // 输入桌面全范围 RGB，输出 NV12 有限范围(16-235)——解码端默认按电视范围解
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCS = {};
            inCS.Usage = 0;          // 0 = RGB 输入
            inCS.RGB_Range = 1;      // 全范围 0-255
            videoContext->VideoProcessorSetStreamColorSpace(videoProcessor.Get(), 0, &inCS);
            videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCS = {};
            outCS.Usage = 1;         // 1 = YCbCr 输出
            outCS.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            videoContext->VideoProcessorSetOutputColorSpace(videoProcessor.Get(), &outCS);

            return true;
        }

        int NvencH264Encoder::Encode(const webrtc::VideoFrame& frame,
            const std::vector<webrtc::VideoFrameType>* frameTypes) {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (!nvencSession)
                return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

            auto buffer = frame.video_frame_buffer();
            uint32_t idx = nextBitstream;

            NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
            params.inputTimeStamp = frame.rtp_timestamp();
            params.inputWidth = buffer->width();
            params.inputHeight = buffer->height();
            params.outputBitstream = bitstreams[idx].ptr;
            params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

            if (frameTypes && !frameTypes->empty() &&
                (*frameTypes)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
                params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            }

            NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
            if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
                auto* d3dBuffer = static_cast<WebrtcD3D11TextureBuffer*>(buffer.get());
                HANDLE h = d3dBuffer->GetSharedHandle();

                // 上游因 keyed-mutex 同步丢失重开了帧通道（generation++），旧的
                // 共享纹理/handle 已全部失效，必须清空缓存用新 handle 重新打开。
                if (channelSync) {
                    uint32_t gen = channelSync->generation.load(std::memory_order_acquire);
                    if (gen != lastSeenGeneration) {
                        lastSeenGeneration = gen;
                        resourceCache.clear();
                    }
                }

                auto& cached = resourceCache[h];
                if (!cached.tex) {
                    // Producer slot handles are NT handles (CreateSharedHandle);
                    // OpenSharedResource1 is required, not legacy OpenSharedResource.
                    Microsoft::WRL::ComPtr<ID3D11Device1> dev1;
                    HRESULT oh = d3dDevice.As(&dev1);
                    if (SUCCEEDED(oh)) oh = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&cached.tex));
                    if (FAILED(oh) || !cached.tex) {
                        LOG_ERROR("[NVENC-H264] OpenSharedResource1 failed hr=0x%08X", (unsigned)oh);
                        return WEBRTC_VIDEO_CODEC_ERROR;
                    }
                    cached.tex.As(&cached.km);
                    // VP 输入视图：BGRA 共享纹理 -> VideoProcessor
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
                    ivDesc.FourCC = 0;
                    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivDesc.Texture2D.MipSlice = 0;
                    HRESULT ivr = videoDevice->CreateVideoProcessorInputView(cached.tex.Get(), vpEnumerator.Get(), &ivDesc, &cached.vpInputView);
                    if (FAILED(ivr) || !cached.vpInputView) {
                        LOG_ERROR("[NVENC-H264] CreateVideoProcessorInputView 失败 hr=0x%08X", (unsigned)ivr);
                        return WEBRTC_VIDEO_CODEC_ERROR;
                    }
                }

                HRESULT hr = cached.km ? cached.km->AcquireSync(1, kVddAcquireTimeoutMs) : static_cast<HRESULT>(E_FAIL);
                if (hr == S_OK) {
                    // DXVA VP：固定功能视频引擎转 BGRA -> NV12（无矩形参数，全图转换）
                    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                    stream.Enable = TRUE;
                    stream.OutputIndex = 0;
                    stream.pInputSurface = cached.vpInputView.Get();
                    HRESULT vbr = videoContext->VideoProcessorBlt(videoProcessor.Get(), inputPool[idx].vpOutputView.Get(), 0, 1, &stream);
                    // Flush 确保 VP 读完共享纹理后再还锁，避免撕裂
                    d3dContext->Flush();
                    if (FAILED(vbr)) {
                        LOG_ERROR("[NVENC-H264] VideoProcessorBlt 失败 hr=0x%08X", (unsigned)vbr);
                        cached.km->ReleaseSync(0);
                        d3dBuffer->FreeSharedSlot();
                        return WEBRTC_VIDEO_CODEC_ERROR;
                    }

                    // VP 已把数据搬进 NV12 池槽，共享纹理立即归还
                    cached.km->ReleaseSync(0);
                    d3dBuffer->FreeSharedSlot();

                    // 映射 NV12 池槽给 NVENC 编码（不再直注共享纹理）
                    map.registeredResource = inputPool[idx].regPtr;
                    nvencFuncs.nvEncMapInputResource(nvencSession, &map);
                    params.inputBuffer = map.mappedResource;
                    params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                    mappedResources[idx] = map.mappedResource;
                    swInputBuffers[idx] = nullptr;
                    pendingInputs[idx].isShared = false;
                }
                else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
                    // 该 slot 自上次 ReleaseSync(0) 后未被驱动重新发布（背压/陈旧 relay）：
                    // 跳过本帧，等下一帧事件重送。不 erase 缓存、不置 reopenRequested、
                    // 不返回 ERROR（避免触发 libwebrtc 编码器重建），静默丢帧即可。
                    return WEBRTC_VIDEO_CODEC_OK;
                }
                else {
                    LOG_ERROR("[NVENC] D3D AcquireSync failed. handle=%p hr=0x%08X", h, (unsigned)hr);
                    // keyed mutex 失效（多半是驱动重建了共享纹理）：丢弃该 handle 的
                    // 缓存，避免后续每帧对旧纹理空等；并请求上游重开通道，让
                    // generation++ 后本编码器清空全部缓存、用新 handle 重建同步。
                    resourceCache.erase(h);
                    if (channelSync) {
                        if (channelSync->generation.load(std::memory_order_acquire) != lastRequestedGeneration) {
                            lastRequestedGeneration = channelSync->generation.load(std::memory_order_acquire);
                            channelSync->reopenRequested.store(1, std::memory_order_release);
                        }
                    }
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
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
                    libyuv::CopyPlane(nv12->DataY(), nv12->StrideY(),
                        (uint8_t*)lockInput.bufferDataPtr, lockInput.pitch, widths, heights);
                    libyuv::CopyPlane(nv12->DataUV(), nv12->StrideUV(),
                        (uint8_t*)lockInput.bufferDataPtr + (heights * lockInput.pitch), lockInput.pitch, widths, heights / 2);
                }
                else {
                    auto i420 = buffer->ToI420();
                    libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
                        i420->DataV(), i420->StrideV(),
                        (uint8_t*)lockInput.bufferDataPtr, lockInput.pitch,
                        (uint8_t*)lockInput.bufferDataPtr + (heights * lockInput.pitch), lockInput.pitch,
                        widths, heights);
                }
                nvencFuncs.nvEncUnlockInputBuffer(nvencSession, swInputBuffer);

                params.inputBuffer = swInputBuffer;
                params.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                mappedResources[idx] = nullptr;
                swInputBuffers[idx] = swInputBuffer;
                pendingInputs[idx].isShared = false;
            }

            NVENCSTATUS err = nvencFuncs.nvEncEncodePicture(nvencSession, &params);
            if (err == NV_ENC_SUCCESS) {
                dtsList.push_back(frame.render_time_ms());
                buffersQueued++;
                if (++nextBitstream == bufCount) nextBitstream = 0;
            }
            else {
                LOG_ERROR("[NVENC] EncodePicture failed: %d", err);
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            GetEncodedPacket(false);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        bool NvencH264Encoder::GetEncodedPacket(bool finalize) {
            if (!buffersQueued) return true;

            uint32_t count = buffersQueued;

            for (uint32_t i = 0; i < count; i++) {
                NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
                lock.outputBitstream = bitstreams[curBitstream].ptr;
                lock.doNotWait = false;

                if (nvencFuncs.nvEncLockBitstream(nvencSession, &lock) == NV_ENC_SUCCESS) {
                    webrtc::EncodedImage image;
                    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                        (uint8_t*)lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes));
                    image._encodedWidth = widths;
                    image._encodedHeight = heights;
                    image.SetRtpTimestamp(static_cast<uint32_t>(lock.outputTimeStamp));

                    if (!dtsList.empty()) {
                        image.capture_time_ms_ = dtsList.front();
                        dtsList.pop_front();
                    }

                    image._frameType = (lock.pictureType == NV_ENC_PIC_TYPE_IDR)
                        ? webrtc::VideoFrameType::kVideoFrameKey
                        : webrtc::VideoFrameType::kVideoFrameDelta;

                    webrtc::CodecSpecificInfo info;
                    info.codecType = webrtc::kVideoCodecH264;
                    info.end_of_picture = true;
                    if (encodedImageCallback) encodedImageCallback->OnEncodedImage(image, &info);

                    nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreams[curBitstream].ptr);
                    if (mappedResources[curBitstream]) {
                        nvencFuncs.nvEncUnmapInputResource(nvencSession, mappedResources[curBitstream]);
                        mappedResources[curBitstream] = nullptr;
                    }
                    // 直注路径：unmap 完成后 NVENC 不再读共享纹理，此时归还 keyed mutex 并释放捕获槽
                    if (pendingInputs[curBitstream].isShared) {
                        if (pendingInputs[curBitstream].km)
                            pendingInputs[curBitstream].km->ReleaseSync(0);
                        if (pendingInputs[curBitstream].buffer) {
                            auto* d3d = static_cast<WebrtcD3D11TextureBuffer*>(pendingInputs[curBitstream].buffer.get());
                            if (d3d) d3d->FreeSharedSlot();
                        }
                        pendingInputs[curBitstream].km.Reset();
                        pendingInputs[curBitstream].buffer = nullptr;
                        pendingInputs[curBitstream].isShared = false;
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

        int NvencH264Encoder::Release() {
            std::lock_guard<std::mutex> lock(nvencApiMutex);
            if (nvencSession) {
                NV_ENC_PIC_PARAMS params = { NV_ENC_PIC_PARAMS_VER };
                params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
                nvencFuncs.nvEncEncodePicture(nvencSession, &params);
                GetEncodedPacket(true);

                for (auto& it : inputPool) {
                    if (it.regPtr) nvencFuncs.nvEncUnregisterResource(nvencSession, it.regPtr);
                }
                for (auto& bs : bitstreams) {
                    if (bs.ptr) nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bs.ptr);
                }

                // 注销直注路径下注册的共享纹理
                for (auto& kv : resourceCache)
                    if (kv.second.regPtr) nvencFuncs.nvEncUnregisterResource(nvencSession, kv.second.regPtr);
                resourceCache.clear();

                // 兜底：销毁时若仍有在途的共享槽未释放，归还 keyed mutex 与捕获槽，避免卡住捕获侧
                for (auto& p : pendingInputs) {
                    if (p.isShared) {
                        if (p.km) p.km->ReleaseSync(0);
                        if (p.buffer) {
                            auto* d3d = static_cast<WebrtcD3D11TextureBuffer*>(p.buffer.get());
                            if (d3d) d3d->FreeSharedSlot();
                        }
                        p.km.Reset();
                        p.buffer = nullptr;
                        p.isShared = false;
                    }
                }
                pendingInputs.clear();

                nvencFuncs.nvEncDestroyEncoder(nvencSession);
                nvencSession = nullptr;
            }
            if (nvVideoCodecHandle) {
                FreeLibrary(nvVideoCodecHandle);
                nvVideoCodecHandle = nullptr;
            }
            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencH264Encoder::RegisterEncodeCompleteCallback(
            webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencH264Encoder::SetChannelSync(std::shared_ptr<VddChannelSync> s) {
            channelSync = std::move(s);
            // 立即同步当前 generation，避免误把注入前的通道当作"已重开"而清空缓存。
            if (channelSync) lastSeenGeneration = channelSync->generation.load(std::memory_order_acquire);
        }

        void NvencH264Encoder::SetRates(const RateControlParameters& parameters) {
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
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastRateChangeTime).count();
            const int64_t MIN_CHANGE_INTERVAL_MS = 500;

            if (elapsed < MIN_CHANGE_INTERVAL_MS &&
                changeRatio < 2.0 && changeRatio > 0.5) {
                return;
            }

            NV_ENC_RECONFIGURE_PARAMS reconfig = { NV_ENC_RECONFIGURE_PARAMS_VER };
            reconfig.resetEncoder = 0;
            reconfig.forceIDR = 0;

            encodeConfig.rcParams.averageBitRate = targetBitrateBps;
            encodeConfig.rcParams.maxBitRate =
                static_cast<uint32_t>(targetBitrateBps * 2);

            initParams.encodeConfig = &encodeConfig;
            reconfig.reInitEncodeParams = initParams;

            NVENCSTATUS status = nvencFuncs.nvEncReconfigureEncoder(nvencSession, &reconfig);
            if (status != NV_ENC_SUCCESS) {
                LOG_ERROR("[NVENC] ReconfigureEncoder failed: %d", status);
                return;
            }
            lastRateChangeTime = now;
            LOG_INFO("[NVENC] 码率重配置: %u bps", targetBitrateBps);
        }

        webrtc::VideoEncoder::EncoderInfo NvencH264Encoder::GetEncoderInfo() const {
            EncoderInfo info;
            info.supports_native_handle = true;
            info.is_hardware_accelerated = true;
            info.implementation_name = "NVENCH264";
            return info;
        }

    } // namespace rtc
} // namespace hope