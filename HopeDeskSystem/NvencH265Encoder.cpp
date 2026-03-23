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
            LOG_INFO("[NVENC] NvencH265Encoder Constructor - ASYNC DROP_FRAME Mode");
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

            if (!InitD3D11() || !InitNvenc(width, height, targetBitrateBps)) return WEBRTC_VIDEO_CODEC_ERROR;

            isRunning = true;
            fetchThread = std::thread(&NvencH265Encoder::FetchThreadFunc, this);
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
                if (desc.VendorId == 0x10DE) { targetAdapter = adapter; break; }
            }
            if (!targetAdapter) return false;
            return SUCCEEDED(D3D11CreateDevice(targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext));
        }

        bool NvencH265Encoder::InitNvenc(int width, int height, uint32_t bitrateBps) {
            nvVideoCodecHandle = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
            if (!nvVideoCodecHandle) return false;
            PNVENCODEAPICREATEINSTANCE nvEncodeAPICreateInstance = (PNVENCODEAPICREATEINSTANCE)GetProcAddress(nvVideoCodecHandle, "NvEncodeAPICreateInstance");
            if (!nvEncodeAPICreateInstance) return false;
            nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            if (nvEncodeAPICreateInstance(&nvencFuncs) != NV_ENC_SUCCESS) return false;

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
            sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            sessionParams.device = d3dDevice.Get();
            sessionParams.apiVersion = NVENCAPI_VERSION;
            if (nvencFuncs.nvEncOpenEncodeSessionEx(&sessionParams, &nvencSession) != NV_ENC_SUCCESS) return false;

            initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initParams.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
            initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
            initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
            initParams.encodeWidth = width; initParams.encodeHeight = height;
            initParams.enablePTD = 1;
            initParams.enableEncodeAsync = 1;

            NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } };
            nvencFuncs.nvEncGetEncodePresetConfigEx(nvencSession, initParams.encodeGUID, initParams.presetGUID, initParams.tuningInfo, &presetConfig);
            encodeConfig = presetConfig.presetCfg;
            encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig.rcParams.averageBitRate = bitrateBps;
            encodeConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrateBps * 2);
            encodeConfig.rcParams.enableAQ = 0;
            encodeConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
            encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
            initParams.encodeConfig = &encodeConfig;

            if (nvencFuncs.nvEncInitializeEncoder(nvencSession, &initParams) != NV_ENC_SUCCESS) return false;

            for (int i = 0; i < MaxBufferCount; i++) {
                completionEvents[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
                NV_ENC_CREATE_BITSTREAM_BUFFER bsParams = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
                nvencFuncs.nvEncCreateBitstreamBuffer(nvencSession, &bsParams);
                bitstreamBuffers[i] = bsParams.bitstreamBuffer;
                NV_ENC_CREATE_INPUT_BUFFER allocInput = { NV_ENC_CREATE_INPUT_BUFFER_VER };
                allocInput.width = width; allocInput.height = height; allocInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
                nvencFuncs.nvEncCreateInputBuffer(nvencSession, &allocInput);
                sysMemBuffers[i] = allocInput.inputBuffer;
            }
            return true;
        }

        int NvencH265Encoder::Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) {
            if (!nvencSession || !isRunning) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

            int bufIdx = -1;
            {
                std::lock_guard<std::mutex> qLock(queueMutex);
                // 主动丢帧：如果 GPU 编码积压超过上限，丢弃此帧
                if (pendingTaskQueue.size() >= MaxBufferCount) {
                    return WEBRTC_VIDEO_CODEC_OK;
                }
                bufIdx = currentBufferIdx;
                currentBufferIdx = (currentBufferIdx + 1) % MaxBufferCount;
            }

            bool forceKeyFrame = (frameTypes && !frameTypes->empty() && (*frameTypes)[0] == webrtc::VideoFrameType::kVideoFrameKey);
            auto buffer = frame.video_frame_buffer();
            bool isNative = (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative);
            int width = buffer->width(); int height = buffer->height();

            NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
            picParams.inputWidth = width; picParams.inputHeight = height;
            picParams.outputBitstream = bitstreamBuffers[bufIdx];
            picParams.inputTimeStamp = frame.render_time_ms();
            picParams.completionEvent = completionEvents[bufIdx];
            if (forceKeyFrame) picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

            {
                std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                if (isNative) {
                    auto* d3dBuffer = static_cast<WebRTCD3D11TextureBuffer*>(buffer.get());
                    HANDLE sharedHandle = d3dBuffer->GetSharedHandle();
                    auto it = resourceCache.find(sharedHandle);
                    if (it == resourceCache.end()) {
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
                        d3dDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&sharedTex));
                        NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
                        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; reg.resourceToRegister = sharedTex.Get();
                        reg.width = width; reg.height = height; reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
                        nvencFuncs.nvEncRegisterResource(nvencSession, &reg);
                        // 修正：将 registeredPtr 改为 registeredResource
                        resourceCache[sharedHandle] = { sharedTex, reg.registeredResource, NV_ENC_BUFFER_FORMAT_NV12 };
                        it = resourceCache.find(sharedHandle);
                    }
                    NV_ENC_MAP_INPUT_RESOURCE mapRes = { NV_ENC_MAP_INPUT_RESOURCE_VER };
                    mapRes.registeredResource = it->second.registeredPtr;
                    nvencFuncs.nvEncMapInputResource(nvencSession, &mapRes);
                    picParams.inputBuffer = mapRes.mappedResource;
                    mappedInputBuffers[bufIdx] = mapRes.mappedResource;
                }
                else {
                    NV_ENC_LOCK_INPUT_BUFFER lockInput = { NV_ENC_LOCK_INPUT_BUFFER_VER };
                    lockInput.inputBuffer = sysMemBuffers[bufIdx];
                    nvencFuncs.nvEncLockInputBuffer(nvencSession, &lockInput);
                    auto nv12 = buffer->GetNV12();
                    libyuv::CopyPlane(nv12->DataY(), nv12->StrideY(), (uint8_t*)lockInput.bufferDataPtr, lockInput.pitch, width, height);
                    libyuv::CopyPlane(nv12->DataUV(), nv12->StrideUV(), (uint8_t*)lockInput.bufferDataPtr + height * lockInput.pitch, lockInput.pitch, width, height / 2);
                    nvencFuncs.nvEncUnlockInputBuffer(nvencSession, sysMemBuffers[bufIdx]);
                    picParams.inputBuffer = sysMemBuffers[bufIdx];
                }
                nvencFuncs.nvEncEncodePicture(nvencSession, &picParams);
            }

            {
                std::lock_guard<std::mutex> qLock(queueMutex);
                // 修正：使用构造函数入队，避开 VideoFrame 构造报错
                pendingTaskQueue.push(PendingTask(bufIdx, frame, mappedInputBuffers[bufIdx], forceKeyFrame));
            }
            queueCv.notify_all();
            return WEBRTC_VIDEO_CODEC_OK;
        }

        void NvencH265Encoder::FetchThreadFunc() {
            while (isRunning) {
                std::unique_lock<std::mutex> qLock(queueMutex);
                queueCv.wait(qLock, [this] { return !pendingTaskQueue.empty() || !isRunning; });
                if (!isRunning && pendingTaskQueue.empty()) break;

                // 修正获取任务方式
                PendingTask task = pendingTaskQueue.front();
                qLock.unlock();

                if (WaitForSingleObject(completionEvents[task.bufIdx], 1000) == WAIT_OBJECT_0) {
                    std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                    NV_ENC_LOCK_BITSTREAM lockBs = { NV_ENC_LOCK_BITSTREAM_VER };
                    lockBs.outputBitstream = bitstreamBuffers[task.bufIdx];
                    lockBs.doNotWait = 1;
                    if (nvencFuncs.nvEncLockBitstream(nvencSession, &lockBs) == NV_ENC_SUCCESS) {
                        webrtc::EncodedImage image;
                        image.SetEncodedData(webrtc::EncodedImageBuffer::Create((uint8_t*)lockBs.bitstreamBufferPtr, lockBs.bitstreamSizeInBytes));
                        image._encodedWidth = task.frame.width(); image._encodedHeight = task.frame.height();
                        image._frameType = (lockBs.pictureType == NV_ENC_PIC_TYPE_IDR || lockBs.pictureType == NV_ENC_PIC_TYPE_I) ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
                        image.SetRtpTimestamp(task.frame.rtp_timestamp());
                        if (encodedImageCallback) {
                            webrtc::CodecSpecificInfo info; info.codecType = webrtc::kVideoCodecH265;
                            encodedImageCallback->OnEncodedImage(image, &info);
                        }
                        nvencFuncs.nvEncUnlockBitstream(nvencSession, bitstreamBuffers[task.bufIdx]);
                    }
                    if (task.mappedInput) nvencFuncs.nvEncUnmapInputResource(nvencSession, task.mappedInput);
                }

                qLock.lock();
                pendingTaskQueue.pop();
                qLock.unlock();
                queueCv.notify_all();
            }
        }

        int NvencH265Encoder::Release() {
            isRunning = false; queueCv.notify_all();
            if (fetchThread.joinable()) fetchThread.join();
            if (nvencSession) {
                std::lock_guard<std::mutex> apiLock(nvencApiMutex);
                for (auto& pair : resourceCache) nvencFuncs.nvEncUnregisterResource(nvencSession, pair.second.registeredPtr);
                for (int i = 0; i < MaxBufferCount; i++) {
                    CloseHandle(completionEvents[i]);
                    nvencFuncs.nvEncDestroyBitstreamBuffer(nvencSession, bitstreamBuffers[i]);
                    nvencFuncs.nvEncDestroyInputBuffer(nvencSession, sysMemBuffers[i]);
                }
                nvencFuncs.nvEncDestroyEncoder(nvencSession);
            }
            if (nvVideoCodecHandle) FreeLibrary(nvVideoCodecHandle);
            return WEBRTC_VIDEO_CODEC_OK;
        }

        int NvencH265Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) { encodedImageCallback = callback; return WEBRTC_VIDEO_CODEC_OK; }
        void NvencH265Encoder::SetRates(const RateControlParameters& parameters) {}
        webrtc::VideoEncoder::EncoderInfo NvencH265Encoder::GetEncoderInfo() const { webrtc::VideoEncoder::EncoderInfo info; info.supports_native_handle = true; info.implementation_name = "NVENCH265"; return info; }
    }
}