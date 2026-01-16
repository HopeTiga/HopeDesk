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
                LOG_WARNING("Initial DXGI setup failed, trying WinLogon desktop...");
                winLogonSwitcher->SwitchToWinLogonDesktop();
                isOnWinLogonDesktop = true;

                if (!initializeDXGI()) {
                    LOG_ERROR("Failed to initialize DXGI on WinLogon desktop");
                    return false;
                }
            }

            // 如果启用了 GPU 转换，初始化 Shader
            if (config.enableGPUYUV) {
                if (!initializeGPUConverter()) {
                    LOG_WARNING("GPU Converter init failed, falling back to CPU BGRA");
                    config.enableGPUYUV = false;
                }
                else {
                    LOG_INFO("GPU YUV Converter initialized");
                }
            }

            LOG_INFO("ScreenCapture initialized. Size: %dx%d, GPU-YUV: %s, enableDirtyRects %s",
                config.width, config.height, config.enableGPUYUV ? "ON" : "OFF", config.enableDirtyRects ? "ON" : "OFF");
            return true;
        }

        bool ScreenCapture::startCapture() {
            if (capturing.load()) return true;

            capturing = true;
            captureThread = std::thread([this]() {
                LOG_INFO("Capture thread started");
                captureThreadFunc();
                LOG_INFO("Capture thread ended");
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

                if (isOnWinLogonDesktop) {

                }
                else if (!success) {
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
            dxgiOutput.As(&dxgiOutput5);

            static bool init = false;
            hr = dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &dxgiDuplication);

            if (FAILED(hr)) {
                if (hr == E_ACCESSDENIED) {
                    if (init) return false;
                    init = true;
                    releaseResourceDXGI();

                    if (!isOnWinLogonDesktop) {
                        winLogonSwitcher->SwitchToWinLogonDesktop();
                        isOnWinLogonDesktop = true;
                    }
                    else {
                        winLogonSwitcher->SwitchToDefaultDesktop();
                        isOnWinLogonDesktop = false;
                    }
                    return initializeDXGI();
                }
                return false;
            }
            init = false;

            DXGI_OUTDUPL_DESC duplDesc;
            dxgiDuplication->GetDesc(&duplDesc);
            config.width = duplDesc.ModeDesc.Width;
            config.height = duplDesc.ModeDesc.Height;

            // 1. 共享纹理 (BGRA)
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

            // 获取共享句柄
            Microsoft::WRL::ComPtr<IDXGIResource> dxgiRes;
            if (SUCCEEDED(sharedTexture.As(&dxgiRes))) {
                dxgiRes->GetSharedHandle(&sharedHandle);
            }

            // 2. CPU 模式 Staging Textures (仅当不使用GPU转换时创建，省显存)
            if (!config.enableGPUYUV) {
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                for (int i = 0; i < NUM_BUFFERS; i++) {
                    d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextures[i]);
                }
            }

            return true;
        }

        bool ScreenCapture::initializeGPUConverter() {
            // Shader 代码 (BGRA -> YUV/NV12)
            const char* shaderSource = R"(
                Texture2D<float4> inputTexture : register(t0);
                RWStructuredBuffer<uint> outputBuffer : register(u0);
                cbuffer Constants : register(b0) {
                    uint width; uint height; uint ySize; uint uvSize;
                }
                
                [numthreads(16, 16, 1)]
                void main(uint3 id : SV_DispatchThreadID) {
                    if (id.x >= width || id.y >= height) return;
                    
                    float4 color = inputTexture.Load(int3(id.x, id.y, 0));
                    float r = color.r * 255.0f;
                    float g = color.g * 255.0f;
                    float b = color.b * 255.0f;
                    
                    // Y Calculation
                    uint y = (uint)((66 * r + 129 * g + 25 * b) / 256 + 16);
                    y = min(y, 255u);
                    
                    // Packed Y writing
                    uint yIndex = id.y * width + id.x;
                    uint arrayIndex = yIndex / 4;
                    uint byteOffset = yIndex % 4;
                    uint shiftAmount = byteOffset * 8;
                    uint mask = 0xFF << shiftAmount;
                    uint value = y << shiftAmount;
                    
                    InterlockedAnd(outputBuffer[arrayIndex], ~mask);
                    InterlockedOr(outputBuffer[arrayIndex], value);
                    
                    // UV Calculation (Subsampled)
                    if ((id.x % 2 == 0) && (id.y % 2 == 0)) {
                        float4 c00 = color;
                        float4 c10 = inputTexture.Load(int3(min(id.x + 1, width - 1), id.y, 0));
                        float4 c01 = inputTexture.Load(int3(id.x, min(id.y + 1, height - 1), 0));
                        float4 c11 = inputTexture.Load(int3(min(id.x + 1, width - 1), min(id.y + 1, height - 1), 0));
                        float4 avg = (c00 + c10 + c01 + c11) * 0.25f * 255.0f;
                        float avgB = avg.b; float avgG = avg.g; float avgR = avg.r;
                        
                        uint u = (uint)((-38 * avgR - 74 * avgG + 112 * avgB) / 256 + 128);
                        uint v = (uint)((112 * avgR - 94 * avgG - 18 * avgB) / 256 + 128);
                        u = clamp(u, 0u, 255u); v = clamp(v, 0u, 255u);
                        
                        uint uvIndex = (id.y / 2) * (width / 2) + (id.x / 2);
                        
                        // U Writing
                        uint uPos = ySize + uvIndex;
                        uint uArrayIndex = uPos / 4;
                        uint uByteOffset = uPos % 4;
                        InterlockedAnd(outputBuffer[uArrayIndex], ~(0xFF << (uByteOffset * 8)));
                        InterlockedOr(outputBuffer[uArrayIndex], u << (uByteOffset * 8));
                        
                        // V Writing
                        uint vPos = ySize + uvSize + uvIndex;
                        uint vArrayIndex = vPos / 4;
                        uint vByteOffset = vPos % 4;
                        InterlockedAnd(outputBuffer[vArrayIndex], ~(0xFF << (vByteOffset * 8)));
                        InterlockedOr(outputBuffer[vArrayIndex], v << (vByteOffset * 8));
                    }
                }
            )";

            Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

            HRESULT hr = D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob);
            if (FAILED(hr)) {
                if (errorBlob) LOG_ERROR("Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
                return false;
            }

            hr = d3dDevice->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &yuvComputeShader);
            if (FAILED(hr)) return false;

            // Buffer 大小: Y + U + V (I420)
            const UINT yuvBufferSize = config.width * config.height * 3 / 2;
            const UINT bufferSizeInUints = (yuvBufferSize + 3) / 4; // 按4字节对齐

            // 1. GPU Output Buffer (UAV)
            D3D11_BUFFER_DESC bufferDesc = {};
            bufferDesc.ByteWidth = bufferSizeInUints * sizeof(UINT);
            bufferDesc.Usage = D3D11_USAGE_DEFAULT;
            bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = sizeof(UINT);
            hr = d3dDevice->CreateBuffer(&bufferDesc, nullptr, &yuvOutputBuffer);
            if (FAILED(hr)) return false;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = bufferSizeInUints;
            hr = d3dDevice->CreateUnorderedAccessView(yuvOutputBuffer.Get(), &uavDesc, &yuvUAV);
            if (FAILED(hr)) return false;

            // 2. Staging Buffers (CPU Read) - 使用多重缓冲
            bufferDesc.Usage = D3D11_USAGE_STAGING;
            bufferDesc.BindFlags = 0;
            bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bufferDesc.MiscFlags = 0;

            // [优化] 循环初始化数组中的所有 Buffer
            for (int i = 0; i < YUV_BUFFERS; i++) {
                hr = d3dDevice->CreateBuffer(&bufferDesc, nullptr, &yuvStagingBuffers[i]);
                if (FAILED(hr)) {
                    LOG_ERROR("Failed to create staging buffer %d", i);
                    return false;
                }
            }
            currentYuvIdx = 0; // 重置索引

            // 3. Constant Buffer
            struct Constants { UINT w; UINT h; UINT ySize; UINT uvSize; };
            Constants consts = { (UINT)config.width, (UINT)config.height, (UINT)(config.width * config.height), (UINT)(config.width * config.height / 4) };

            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = (sizeof(Constants) + 15) / 16 * 16; // 16字节对齐
            cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA cbData = { &consts, 0, 0 };
            hr = d3dDevice->CreateBuffer(&cbDesc, &cbData, &yuvConstantBuffer);
            if (FAILED(hr)) return false;

            return true;
        }

        bool ScreenCapture::captureFrame() {
            if (!dxgiDuplication) {
                invalidCallDxgi++;
                if (invalidCallDxgi >= 2) {
                    bool targetWinLogon = !desktopSwitchInProgress;
                    releaseResourceDXGI();

                    if (targetWinLogon) winLogonSwitcher->SwitchToWinLogonDesktop();
                    else winLogonSwitcher->SwitchToDefaultDesktop();

                    initializeDXGI();
                    // 如果切了桌面，必须重新初始化 Shader 资源，因为 Device 可能变了
                    if (config.enableGPUYUV) initializeGPUConverter();

                    desktopSwitchInProgress = targetWinLogon;
                    invalidCallDxgi = 0;
                }
                return false;
            }
            invalidCallDxgi = 0;

            HRESULT hr;
            Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;

            hr = dxgiDuplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
            if (FAILED(hr)) {
                handleCaptureError(hr);
                return false;
            }

            struct FrameReleaser {
                IDXGIOutputDuplication* dup;
                ~FrameReleaser() { if (dup) dup->ReleaseFrame(); }
            } releaser{ dxgiDuplication.Get() };

            invalidCallCount = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTexture;
            if (FAILED(desktopResource.As(&acquiredTexture))) return false;

            // --- 脏矩形处理 ---
            bool needsFullFrame = dirtyTracker->fullFrameRequired;
            if (config.enableDirtyRects) {
                if (frameInfo.TotalMetadataBufferSize > 0) {
                    ProcessDirtyRects(&frameInfo, acquiredTexture.Get(), sharedTexture.Get());
                    needsFullFrame = dirtyTracker->fullFrameRequired;
                }
                if (needsFullFrame) {
                    d3dContext->CopyResource(sharedTexture.Get(), acquiredTexture.Get());
                    dirtyTracker->fullFrameRequired = false;
                }
            }
            else {
                d3dContext->CopyResource(sharedTexture.Get(), acquiredTexture.Get());
            }

            // --- 分支处理 ---
            return processFrame(sharedTexture.Get());
        }

        bool ScreenCapture::processFrame(ID3D11Texture2D* texture) {
            if (config.enableGPUYUV && yuvComputeShader) {
                return processFrameGPU_YUV(texture);
            }
            else {
                return processFrameCPU_BGRA(texture);
            }
        }

        // 路径 A: CPU 直通 (BGRA)
        bool ScreenCapture::processFrameCPU_BGRA(ID3D11Texture2D* texture) {
            if (!texture || !d3dContext) return false;

            d3dContext->CopyResource(stagingTextures[currentTexture].Get(), texture);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(d3dContext->Map(stagingTextures[currentTexture].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }

            if (dataHandle) {
                // isYUV = false
                dataHandle(
                    reinterpret_cast<const uint8_t*>(mapped.pData),
                    static_cast<int>(mapped.RowPitch),
                    config.width,
                    config.height,
                    false
                );
            }

            d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);
            currentTexture = (currentTexture + 1) % NUM_BUFFERS;
            return true;
        }

        // 路径 B: GPU 转换 (YUV)
        // [优化] 引入 Ring Buffer 机制，解除 CPU/GPU 串行等待
        bool ScreenCapture::processFrameGPU_YUV(ID3D11Texture2D* texture) {
            if (!texture || !d3dContext || !yuvComputeShader) return false;

            // 1. 设置 Shader 资源 (SRV)
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

            // 2. Dispatch (GPU 开始计算当前帧)
            UINT dispatchX = (config.width + 15) / 16;
            UINT dispatchY = (config.height + 15) / 16;
            d3dContext->Dispatch(dispatchX, dispatchY, 1);

            // 3. 解绑 UAV 避免冲突 (必须在 Dispatch 后做)
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            ID3D11ShaderResourceView* nullSRV = nullptr;
            d3dContext->CSSetShaderResources(0, 1, &nullSRV);

            // 4. 将计算结果拷贝到 "当前写" 的 Staging Buffer
            // GPU 在后台执行这个拷贝，不会阻塞 CPU
            d3dContext->CopyResource(yuvStagingBuffers[currentYuvIdx].Get(), yuvOutputBuffer.Get());

            // 5. 计算读取索引 (读取 N-1 或 N-2 帧的数据)
            int readIdx = (currentYuvIdx + 1) % YUV_BUFFERS;

            // 更新当前索引指向下一帧
            currentYuvIdx = readIdx;

            // 6. Map 读取 (读取的是以前的帧，GPU 此时很可能已经写完了，Map 不会等待)
            D3D11_MAPPED_SUBRESOURCE mapped;
            // 如果是刚启动，前几帧 buffer 可能是空的，Map 出来是黑屏，这是正常的
            if (SUCCEEDED(d3dContext->Map(yuvStagingBuffers[readIdx].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                if (dataHandle) {
                    // isYUV = true
                    // 对于 YUV420p，width 就是 Y 分量的 stride
                    dataHandle(
                        reinterpret_cast<const uint8_t*>(mapped.pData),
                        config.width, // stride
                        config.width,
                        config.height,
                        true
                    );
                }
                d3dContext->Unmap(yuvStagingBuffers[readIdx].Get(), 0);
                return true;
            }
            return false;
        }

        // ... DirtyRects 相关函数 (保持原样) ...
        void ScreenCapture::ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture) {
            if (moveRect->SourcePoint.x == moveRect->DestinationRect.left &&
                moveRect->SourcePoint.y == moveRect->DestinationRect.top) return;

            D3D11_BOX srcBox;
            srcBox.left = moveRect->SourcePoint.x;
            srcBox.top = moveRect->SourcePoint.y;
            srcBox.right = srcBox.left + (moveRect->DestinationRect.right - moveRect->DestinationRect.left);
            srcBox.bottom = srcBox.top + (moveRect->DestinationRect.bottom - moveRect->DestinationRect.top);
            srcBox.front = 0; srcBox.back = 1;

            d3dContext->CopySubresourceRegion(destTexture, 0,
                moveRect->DestinationRect.left, moveRect->DestinationRect.top, 0,
                sourceTexture, 0, &srcBox);
        }

        void ScreenCapture::ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture) {
            dirtyTracker->Reset();
            if (dirtyTracker->metadataBuffer.size() < frameInfo->TotalMetadataBufferSize) {
                dirtyTracker->metadataBuffer.resize(frameInfo->TotalMetadataBufferSize);
            }
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
            std::vector<RECT> res;
            res.reserve(count);
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
                    if (config.enableGPUYUV) initializeGPUConverter();

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
            dxgiDuplication.Reset();
            for (auto& st : stagingTextures) st.Reset();
            sharedTexture.Reset();
            sharedHandle = nullptr;

            // 释放 Shader 资源
            yuvComputeShader.Reset();
            yuvOutputBuffer.Reset();
            // [优化] 释放所有缓冲
            for (int i = 0; i < YUV_BUFFERS; i++) {
                yuvStagingBuffers[i].Reset();
            }
            yuvConstantBuffer.Reset();
            yuvUAV.Reset();
        }
    }
}