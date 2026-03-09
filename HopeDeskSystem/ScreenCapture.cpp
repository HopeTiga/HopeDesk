#include "ScreenCapture.h"
#include <algorithm>
#include <vector>
#include <d3dcompiler.h>
#include "Utils.h" 

#pragma comment(lib, "d3dcompiler.lib")

namespace hope {
    namespace rtc {

        struct DirtyRegionTracker {
            std::vector<RECT> dirtyRects;
            std::vector<uint8_t> metadataBuffer;
            bool fullFrameRequired = true;

            DirtyRegionTracker() {
                metadataBuffer.reserve(32768);
                dirtyRects.reserve(64);
            }
            void Reset() { dirtyRects.clear(); }
        };

        ScreenCapture::ScreenCapture() {
            winLogonSwitcher = std::make_unique<WinLogon>();
            winLogonSwitcher->SwitchToDefaultDesktop();
            dirtyTracker = std::make_unique<DirtyRegionTracker>();
        }

        ScreenCapture::~ScreenCapture() {
            stopCapture();
            if (isOnWinLogonDesktop && winLogonSwitcher) {
                winLogonSwitcher->SwitchToDefaultDesktop();
            }
            releaseResources();
        }

        bool ScreenCapture::initialize() {

            LOG_INFO("=== Starting ScreenCapture (Full) ===");

            if (!initializeDXGI()) {

                winLogonSwitcher->SwitchToWinLogonDesktop();

                desktopSwitchInProgress = true;

                if (!initializeDXGI()) {

                    LOG_ERROR("Failed to initialize DXGI on WinLogon desktop");

                    return false;
                }

            }

            if (config.levels == CaptureLevels::PRO) {
            
                if (!initializeProcessor()) {

                    LOG_WARNING("PRO Converter init failed, falling back to CPU BGRA");

                    config.levels = CaptureLevels::GPU;

                }
                else {
                
                    config.uselevels = CaptureLevels::PRO;

                }


            }

            if (config.levels == CaptureLevels::GPU) {

                if (!initializeGPUConverter()) {

                    LOG_WARNING("GPU Converter init failed, falling back to CPU BGRA");

                    config.levels = CaptureLevels::CPU;

                    config.uselevels = CaptureLevels::CPU;

                }
                else {

                    config.uselevels = CaptureLevels::GPU;

                }
            }

            std::string levels;

            switch (static_cast<int>(config.uselevels)) {
            case 0:
                levels = "CaptureLevels::CPU";
                break;
            case 1:
                levels = "CaptureLevels::GPU";
                break;
            case 2:
                levels = "CaptureLevels::PRO";
                break;
            default:
                levels = "unknown";
                break;
            }

             LOG_INFO("CaptureLevels: %s", levels.c_str());

            return true;
        }

        bool ScreenCapture::startCapture() {

            if (capturing.load()) return true;

            capturing = true;

            captureThread = std::thread([this]() {

                captureThreadFunc();

                });

            return true;
        }

        void ScreenCapture::stopCapture() {

            if (!capturing.load()) return;

            capturing = false;

            if (captureThread.joinable()) captureThread.join();

        }

        void ScreenCapture::captureThreadFunc() {

            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            while (capturing.load()) {

                bool success = captureFrame();

                if (!success && !isOnWinLogonDesktop) {

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));

                }

            }

        }

        bool ScreenCapture::initializeDXGI() {

            HRESULT hr;

            D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

            UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

            D3D_FEATURE_LEVEL featureLevel;

            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                &d3dDevice, &featureLevel, &d3dContext);

            if (FAILED(hr)) return false;

            d3dDevice.As(&d3dDevice1);

            d3dContext.As(&d3dContext1);

            d3dDevice.As(&dxgiDevice);

            dxgiDevice->GetAdapter(&dxgiAdapter);

            dxgiAdapter->EnumOutputs(0, &dxgiOutput);

            dxgiOutput.As(&dxgiOutput1);

            static bool init = false;

            hr = dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &dxgiDuplication);

            if (FAILED(hr)) {

                handleCaptureError(hr);

                return false;
            }

            init = false;

            DXGI_OUTDUPL_DESC duplDesc;

            dxgiDuplication->GetDesc(&duplDesc);

            config.width = duplDesc.ModeDesc.Width;

            config.height = duplDesc.ModeDesc.Height;

            D3D11_TEXTURE2D_DESC desc = {};

            desc.Width = config.width;

            desc.Height = config.height;

            desc.MipLevels = 1;

            desc.ArraySize = 1;

            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

            desc.SampleDesc.Count = 1;

            desc.Usage = D3D11_USAGE_DEFAULT;

            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

            if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &sharedTexture))) return false;

            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            for (int i = 0; i < YUV_BUFFERS; i++) {
                d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextures[i]);
            }

            if (!framePool) {
                framePool = std::make_shared<FramePool>();
            }

            return true;
        }


        bool ScreenCapture::initializeGPUConverter() {

            const char* shaderSource = R"(
                Texture2D<float4> inputTexture : register(t0);
                RWStructuredBuffer<uint> outputBuffer : register(u0);
                cbuffer Constants : register(b0) { uint width; uint height; uint ySize; uint uvSize; }
                uint GetY(float4 c) { return ((66 * (uint)(c.r * 255.0f) + 129 * (uint)(c.g * 255.0f) + 25 * (uint)(c.b * 255.0f) + 128) >> 8) + 16; }
                uint GetU(float4 c) { return ((-38 * (uint)(c.r * 255.0f) - 74 * (uint)(c.g * 255.0f) + 112 * (uint)(c.b * 255.0f) + 128) >> 8) + 128; }
                uint GetV(float4 c) { return ((112 * (uint)(c.r * 255.0f) - 94 * (uint)(c.g * 255.0f) - 18 * (uint)(c.b * 255.0f) + 128) >> 8) + 128; }
                [numthreads(32, 2, 1)]
                void main(uint3 id : SV_DispatchThreadID) {
                    uint startX = id.x * 4;
                    uint y = id.y;
                    if (startX >= width || y >= height) return;
                    float4 c0 = inputTexture.Load(int3(startX + 0, y, 0));
                    float4 c1 = inputTexture.Load(int3(startX + 1, y, 0));
                    float4 c2 = inputTexture.Load(int3(startX + 2, y, 0));
                    float4 c3 = inputTexture.Load(int3(startX + 3, y, 0));
                    uint y0 = GetY(c0); uint y1 = GetY(c1); uint y2 = GetY(c2); uint y3 = GetY(c3);
                    outputBuffer[(y * width + startX) >> 2] = (y3 << 24) | (y2 << 16) | (y1 << 8) | y0;
                    if ((y & 1) == 0 && (y + 1 < height)) {
                        float4 n0 = inputTexture.Load(int3(startX + 0, y + 1, 0));
                        float4 n1 = inputTexture.Load(int3(startX + 1, y + 1, 0));
                        float4 n2 = inputTexture.Load(int3(startX + 2, y + 1, 0));
                        float4 n3 = inputTexture.Load(int3(startX + 3, y + 1, 0));
                        float4 avg0 = (c0 + c1 + n0 + n1) * 0.25f;
                        float4 avg1 = (c2 + c3 + n2 + n3) * 0.25f;
                        uint u0 = GetU(avg0); uint v0 = GetV(avg0);
                        uint u1 = GetU(avg1); uint v1 = GetV(avg1);
                        uint uvRow = y >> 1; uint uvCol = startX >> 1;
                        uint uPos = ySize + uvRow * (width >> 1) + uvCol;
                        uint uIdx = uPos >> 2; uint uShift = (uPos & 3) << 3;
                        InterlockedAnd(outputBuffer[uIdx], ~(0xFFFF << uShift));
                        InterlockedOr(outputBuffer[uIdx], ((u1 << 8) | u0) << uShift);
                        uint vPos = ySize + uvSize + uvRow * (width >> 1) + uvCol;
                        uint vIdx = vPos >> 2; uint vShift = (vPos & 3) << 3;
                        InterlockedAnd(outputBuffer[vIdx], ~(0xFFFF << vShift));
                        InterlockedOr(outputBuffer[vIdx], ((v1 << 8) | v0) << vShift);
                    }
                }
            )";

            Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob, errorBlob;
            HRESULT hr = D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob);
            if (FAILED(hr)) return false;
            d3dDevice->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &yuvComputeShader);

            const UINT yuvBufferSize = config.width * config.height * 3 / 2;
            const UINT bufferSizeInUints = (yuvBufferSize + 3) / 4;

            bufferDesc = {};
            bufferDesc.ByteWidth = bufferSizeInUints * sizeof(UINT);
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = sizeof(UINT);
            d3dDevice->CreateBuffer(&bufferDesc, nullptr, &yuvOutputBuffer);

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = bufferSizeInUints;
            d3dDevice->CreateUnorderedAccessView(yuvOutputBuffer.Get(), &uavDesc, &yuvUAV);

            bufferDesc.Usage = D3D11_USAGE_STAGING;
            bufferDesc.BindFlags = 0;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bufferDesc.MiscFlags = 0;

            // 一次性建立 buffer * 3 的倍数的帧进队列
            for (int i = 0; i < YUV_BUFFERS * 3; i++) {
                auto newBuf = std::make_shared<YuvStagingBuffer>();
                if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, &newBuf->buffer))) return false;
                newBuf->isMapped = false;
                newBuf->mappedData = nullptr;
                framePool->yuvQueue.enqueue(newBuf);
            }

            struct Constants { UINT w; UINT h; UINT ySize; UINT uvSize; };
            Constants consts = { (UINT)config.width, (UINT)config.height, (UINT)(config.width * config.height), (UINT)(config.width * config.height / 4) };
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = (sizeof(Constants) + 15) / 16 * 16;
            cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA cbData = { &consts, 0, 0 };
            d3dDevice->CreateBuffer(&cbDesc, &cbData, &yuvConstantBuffer);

            return true;
        }

        bool ScreenCapture::initializeProcessor()
        {

            if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&proVideoDevice)))) {
                LOG_ERROR("Failed to query ID3D11VideoDevice from d3dDevice");
                return false;
            }

            if (FAILED(d3dContext->QueryInterface(IID_PPV_ARGS(&proVideoContext)))) {
                LOG_ERROR("Failed to query ID3D11VideoContext from d3dContext");
                return false;
            }

            BOOL supported = false;
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
            contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            contentDesc.InputWidth = config.width;
            contentDesc.InputHeight = config.height;
            contentDesc.OutputWidth = config.width;
            contentDesc.OutputHeight = config.height;
            contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            if (FAILED(proVideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &proVideoProcessorEnum))) {
                LOG_ERROR("Failed CreateVideoProcessorEnumerator");
                return false;
            }

            UINT flags = 0;
            if (FAILED(proVideoProcessorEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags))) return false;
            if ((flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
                return false; // Կ֧ VP  NV12
            }

            // 3.  Processor
            if (FAILED(proVideoDevice->CreateVideoProcessor(proVideoProcessorEnum.Get(), 0, &proVideoProcessor))) return false;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = config.width;
            texDesc.Height = config.height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_NV12;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(d3dDevice->CreateTexture2D(&texDesc, nullptr, &proOutputTex))) return false;

            // 3. ؼʼ CPU ضõ Staging 
            texDesc.Usage = D3D11_USAGE_STAGING;
            texDesc.BindFlags = 0; // Staging Ҫ BindFlags
            texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            texDesc.MiscFlags = 0; // ֮ǰ Flags

            // 一次性建立 buffer * 3 的倍数的帧进队列
            for (int i = 0; i < YUV_BUFFERS * 3; i++) {
                auto newBuf = std::make_shared<Nv12TextureBuffer>();
                if (FAILED(d3dDevice->CreateTexture2D(&texDesc, nullptr, &newBuf->buffer))) {
                    LOG_ERROR("Failed to create Pro staging texture %d", i);
                    return false;
                }
                newBuf->isMapped = false;
                framePool->nv12Queue.enqueue(newBuf);
            }

            return true;
        }

        bool ScreenCapture::captureFrame() {

            if (!dxgiDuplication) {
                handleCaptureError(DXGI_ERROR_ACCESS_LOST);
                return false;
            }

            HRESULT hr;
            Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            hr = dxgiDuplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
            if (FAILED(hr)) { handleCaptureError(hr); return false; }

            struct FrameReleaser {
                IDXGIOutputDuplication* dup;
                ~FrameReleaser() { if (dup) dup->ReleaseFrame(); }
            } releaser{ dxgiDuplication.Get() };

            Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTexture;
            if (FAILED(desktopResource.As(&acquiredTexture))) return false;

            return processFrame(acquiredTexture.Get());
        }

        bool ScreenCapture::processFrame(ID3D11Texture2D* texture) {

            if (config.uselevels == CaptureLevels::PRO) return processFramePro(texture);
            else if (config.uselevels == CaptureLevels::GPU && yuvComputeShader) return processFrameGPU(texture);
            return processFrameCPU(texture);
        }

        bool ScreenCapture::processFrameCPU(ID3D11Texture2D* texture) {

            if (!texture || !d3dContext) return false;
            d3dContext->CopyResource(stagingTextures[currentTexture].Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(d3dContext->Map(stagingTextures[currentTexture].Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

            if (dataHandle) {
                // CPU 模式完全同步调用，回调传 nullptr
                dataHandle(reinterpret_cast<const uint8_t*>(mapped.pData), config.width, config.height, nullptr, mapped.RowPitch, CaptureLevels::CPU);
            }
            d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);
            currentTexture = (currentTexture + 1) % YUV_BUFFERS;
            return true;
        }

        bool ScreenCapture::processFrameGPU(ID3D11Texture2D* texture) {
            if (!texture || !d3dContext || !yuvComputeShader || !framePool) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            d3dDevice->CreateShaderResourceView(texture, &srvDesc, &srv);

            d3dContext->CSSetShader(yuvComputeShader.Get(), nullptr, 0);
            d3dContext->CSSetShaderResources(0, 1, srv.GetAddressOf());
            d3dContext->CSSetUnorderedAccessViews(0, 1, yuvUAV.GetAddressOf(), nullptr);
            d3dContext->CSSetConstantBuffers(0, 1, yuvConstantBuffer.GetAddressOf());

            UINT dispatchX = (config.width + 127) / 128;
            UINT dispatchY = (config.height + 1) / 2;

            d3dContext->Dispatch(dispatchX, dispatchY, 1);

            ID3D11UnorderedAccessView* nullUAV = nullptr;
            d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            ID3D11ShaderResourceView* nullSRV = nullptr;
            d3dContext->CSSetShaderResources(0, 1, &nullSRV);

            std::shared_ptr<YuvStagingBuffer> targetBuffer;

            // 直接尝试出列，没有可用的就丢帧直接跳过
            if (!framePool->yuvQueue.try_dequeue(targetBuffer)) {
                return true;
            }

            // 在主线程执行安全的 Unmap
            if (targetBuffer->isMapped) {
                d3dContext->Unmap(targetBuffer->buffer.Get(), 0);
                targetBuffer->isMapped = false;
                targetBuffer->mappedData = nullptr;
            }

            d3dContext->CopyResource(targetBuffer->buffer.Get(), yuvOutputBuffer.Get());

            if (SUCCEEDED(d3dContext->Map(targetBuffer->buffer.Get(), 0, D3D11_MAP_READ, 0, &targetBuffer->mappedSubresource))) {
                targetBuffer->mappedData = static_cast<uint8_t*>(targetBuffer->mappedSubresource.pData);
                targetBuffer->isMapped = true;

                if (dataHandle) {
                    // 使用 weak_ptr 控制生命周期，析构时切断
                    std::weak_ptr<FramePool> weakPool = framePool;
                    auto releaseCb = [weakPool, targetBuffer]() {
                        if (auto pool = weakPool.lock()) {
                            pool->yuvQueue.enqueue(targetBuffer);
                        }
                    };
                    dataHandle(targetBuffer->mappedData, config.width, config.height, releaseCb, config.width, CaptureLevels::GPU);
                }
                else {
                    framePool->yuvQueue.enqueue(targetBuffer);
                }
            }
            else {
                framePool->yuvQueue.enqueue(targetBuffer);
            }

            return true;
        }

        bool ScreenCapture::processFramePro(ID3D11Texture2D* texture)
        {
            if (!framePool) return false;
            // 1.  VideoProcessor ͼ (View)
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDesc.Texture2D.MipSlice = 0;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
            if (FAILED(proVideoDevice->CreateVideoProcessorInputView(texture, proVideoProcessorEnum.Get(), &inputDesc, &inputView))) return false;

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
            //  proOutputTex  Default Usage (GPU)
            if (FAILED(proVideoDevice->CreateVideoProcessorOutputView(proOutputTex.Get(), proVideoProcessorEnum.Get(), &outputDesc, &outputView))) return false;

            // 2. ִӲת (RGBA -> NV12)
            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView.Get();
            HRESULT hr = proVideoContext->VideoProcessorBlt(proVideoProcessor.Get(), outputView.Get(), 0, 1, &stream);
            if (FAILED(hr)) return false;

            std::shared_ptr<Nv12TextureBuffer> targetFrame;

            // 同样直接尝试出列
            if (!framePool->nv12Queue.try_dequeue(targetFrame)) {
                return true;
            }

            if (targetFrame->isMapped) {
                d3dContext->Unmap(targetFrame->buffer.Get(), 0);
                targetFrame->isMapped = false;
                targetFrame->mappedSubresource.pData = nullptr;
            }

            d3dContext->CopyResource(targetFrame->buffer.Get(), proOutputTex.Get());

            if (SUCCEEDED(d3dContext->Map(targetFrame->buffer.Get(), 0, D3D11_MAP_READ, 0, &targetFrame->mappedSubresource))) {
                
                targetFrame->isMapped = true;

                if (dataHandle) {
                    std::weak_ptr<FramePool> weakPool = framePool;
                    auto releaseCb = [weakPool, targetFrame]() {
                        if (auto pool = weakPool.lock()) {
                            pool->nv12Queue.enqueue(targetFrame);
                        }
                    };
                    dataHandle(reinterpret_cast<const uint8_t*>(targetFrame->mappedSubresource.pData),
                        config.width, config.height, releaseCb, targetFrame->mappedSubresource.RowPitch, CaptureLevels::PRO);
                }
                else {
                    framePool->nv12Queue.enqueue(targetFrame);
                }

            }
            else {
                framePool->nv12Queue.enqueue(targetFrame);
            }
        
            return true;
        }

        void ScreenCapture::ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture) {
            if (moveRect->SourcePoint.x == moveRect->DestinationRect.left && moveRect->SourcePoint.y == moveRect->DestinationRect.top) return;
            D3D11_BOX srcBox;
            srcBox.left = moveRect->SourcePoint.x; srcBox.top = moveRect->SourcePoint.y;
            srcBox.right = srcBox.left + (moveRect->DestinationRect.right - moveRect->DestinationRect.left);
            srcBox.bottom = srcBox.top + (moveRect->DestinationRect.bottom - moveRect->DestinationRect.top);
            srcBox.front = 0; srcBox.back = 1;
            d3dContext->CopySubresourceRegion(destTexture, 0, moveRect->DestinationRect.left, moveRect->DestinationRect.top, 0, sourceTexture, 0, &srcBox);
        }

        void ScreenCapture::ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture) {
            dirtyTracker->Reset();
            if (dirtyTracker->metadataBuffer.size() < frameInfo->TotalMetadataBufferSize) dirtyTracker->metadataBuffer.resize(frameInfo->TotalMetadataBufferSize);
            if (frameInfo->TotalMetadataBufferSize == 0) return;
            UINT moveRectSize = 0;
            DXGI_OUTDUPL_MOVE_RECT* moveRects = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(dirtyTracker->metadataBuffer.data());
            if (SUCCEEDED(dxgiDuplication->GetFrameMoveRects(frameInfo->TotalMetadataBufferSize, moveRects, &moveRectSize))) {
                UINT count = moveRectSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                for (UINT i = 0; i < count; ++i) ProcessMoveRect(sourceTexture, &moveRects[i], destTexture);
            }
            UINT dirtyRectSize = 0;
            RECT* dirtyRects = reinterpret_cast<RECT*>(dirtyTracker->metadataBuffer.data() + moveRectSize);
            UINT remainingSize = frameInfo->TotalMetadataBufferSize - moveRectSize;
            if (SUCCEEDED(dxgiDuplication->GetFrameDirtyRects(remainingSize, dirtyRects, &dirtyRectSize))) {
                UINT count = dirtyRectSize / sizeof(RECT);
                auto merged = MergeDirtyRects(dirtyRects, count);
                for (const auto& rect : merged) {
                    D3D11_BOX box{ (UINT)rect.left, (UINT)rect.top, 0, (UINT)rect.right, (UINT)rect.bottom, 1 };
                    d3dContext->CopySubresourceRegion(destTexture, 0, rect.left, rect.top, 0, sourceTexture, 0, &box);
                }
            }
        }
        std::vector<RECT> ScreenCapture::MergeDirtyRects(RECT* rects, UINT count) {

            if (count == 0) return {};
            long totalArea = 0;
            std::vector<RECT> res; res.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                totalArea += (rects[i].right - rects[i].left) * (rects[i].bottom - rects[i].top);
                res.push_back(rects[i]);
            }
            if (totalArea > (config.width * config.height * 0.4)) {
                dirtyTracker->fullFrameRequired = true;
                return {};
            }
            return res;
        }
        void ScreenCapture::handleCaptureError(HRESULT hr) {
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                invalidCallCount++;
                if (invalidCallCount >= 2) {
                    bool targetWinLogon = !desktopSwitchInProgress;
                    releaseResourceDXGI();
                    if (targetWinLogon) winLogonSwitcher->SwitchToWinLogonDesktop();
                    else winLogonSwitcher->SwitchToDefaultDesktop();
                    initializeDXGI();
                    if (config.uselevels == CaptureLevels::PRO) initializeProcessor();
                    if (config.uselevels == CaptureLevels::GPU) initializeGPUConverter();
                    desktopSwitchInProgress = targetWinLogon;
                    invalidCallCount = 0;
                }
            }
        }

        void ScreenCapture::releaseResources() {
            releaseResourceDXGI();
            d3dContext.Reset(); d3dContext1.Reset();
            d3dDevice.Reset(); d3dDevice1.Reset();
            dxgiAdapter.Reset(); dxgiDevice.Reset();
            dxgiOutput.Reset(); dxgiOutput1.Reset(); dxgiOutput5.Reset();
        }

        void ScreenCapture::releaseResourceDXGI() {
            // 切断 WebRTC 等其他线程的队列连结，这步非常关键
            if (framePool) {
                framePool.reset(); 
            }

            dxgiDuplication.Reset();

            for (auto& st : stagingTextures) st.Reset();
            sharedTexture.Reset();
            sharedHandle = nullptr;
            yuvComputeShader.Reset();
            yuvOutputBuffer.Reset();
            yuvConstantBuffer.Reset();
            yuvUAV.Reset();

            proVideoDevice.Reset();
            proVideoContext.Reset();
            proVideoProcessor.Reset();
            proVideoProcessorEnum.Reset();
            proOutputTex.Reset(); 
        }
    }
}