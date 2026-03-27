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

            for (int i = 0; i < YUV_BUFFERS; i++) {
                hwSharedBusy[i].store(false);
            }
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
                    LOG_WARNING("PRO Converter init failed, falling back to GPU");
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
            case 0: levels = "CaptureLevels::CPU"; break;
            case 1: levels = "CaptureLevels::GPU"; break;
            case 2: levels = "CaptureLevels::PRO"; break;
            default: levels = "unknown"; break;
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

            Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
            hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
            if (FAILED(hr)) {
                LOG_ERROR("[ScreenCapture] CreateDXGIFactory1 失败, hr=0x%08X", hr);
                return false;
            }

            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> targetAdapter;
            canZeroCopy = false;
            // ==================== 智能兼容探测 ====================
            // 1. 如果意图使用硬件编码，尝试寻找"挂载了显示器的 NVIDIA 显卡"
            if (gpuDataHandle) {
                LOG_INFO("[ScreenCapture] 开始枚举显卡，寻找支持零拷贝的 NVIDIA 显卡...");

                for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC1 desc;
                    adapter->GetDesc1(&desc);

                    LOG_INFO("[ScreenCapture] 发现适配器 [%u]: %ls (VendorId: 0x%04X, DeviceId: 0x%04X)",
                        i, desc.Description, desc.VendorId, desc.DeviceId);

                    if (desc.VendorId == 0x10DE) { // NVIDIA
                        // 检查所有输出，而不仅仅是第一个
                        Microsoft::WRL::ComPtr<IDXGIOutput> out;
                        bool hasOutput = false;
                        for (UINT j = 0; adapter->EnumOutputs(j, &out) != DXGI_ERROR_NOT_FOUND; ++j) {
                            DXGI_OUTPUT_DESC outDesc;
                            out->GetDesc(&outDesc);
                            LOG_INFO("[ScreenCapture]   输出 [%u]: %ls", j, outDesc.DeviceName);
                            hasOutput = true;
                        }

                        if (hasOutput) {
                            targetAdapter = adapter;
                            canZeroCopy = true;
                            LOG_INFO("[ScreenCapture] ✓ 选中 NVIDIA 显卡 [%u] 带有显示输出，启用硬件零拷贝通道！", i);
                            break;
                        }
                        else {
                            LOG_WARNING("[ScreenCapture] NVIDIA 显卡 [%u] 没有显示输出，可能是 Optimus 笔记本或显示器未连接", i);
                        }
                    }
                }
            }
            else {
                LOG_INFO("[ScreenCapture] gpuDataHandle 为空，跳过 NVIDIA 零拷贝检测");
            }

            // 2. 创建 D3D 设备
            if (!targetAdapter) {
                LOG_WARNING("[ScreenCapture] 未满足 NVIDIA 零拷贝条件，退避到默认软捕获兼容模式...");
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                    featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                    &d3dDevice, &featureLevel, &d3dContext);
            }
            else {
                hr = D3D11CreateDevice(targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags,
                    featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                    &d3dDevice, &featureLevel, &d3dContext);
            }

            if (FAILED(hr)) {
                LOG_ERROR("[ScreenCapture] D3D11Device 创建失败, hr=0x%08X", hr);
                return false;
            }

            d3dDevice.As(&d3dDevice1);
            d3dContext.As(&d3dContext1);
            d3dDevice.As(&dxgiDevice);

            // 注意：GetAdapter 返回 IDXGIAdapter，不是 IDXGIAdapter1
            Microsoft::WRL::ComPtr<IDXGIAdapter> tempAdapter;
            hr = dxgiDevice->GetAdapter(&tempAdapter);
            if (FAILED(hr)) {
                LOG_ERROR("[ScreenCapture] GetAdapter 失败, hr=0x%08X", hr);
                return false;
            }

            // 转换为 IDXGIAdapter1 以使用 GetDesc1，或者直接使用 GetDesc
            DXGI_ADAPTER_DESC actualDesc;
            tempAdapter->GetDesc(&actualDesc);
            LOG_INFO("[ScreenCapture] D3D 设备创建成功，实际使用适配器: %ls (VendorId: 0x%04X)",
                actualDesc.Description, actualDesc.VendorId);

            // 检查是否是我们期望的 NVIDIA 显卡
            if (targetAdapter && actualDesc.VendorId != 0x10DE) {
                LOG_WARNING("[ScreenCapture] 警告：期望使用 NVIDIA 显卡，但实际创建的是其他显卡！");
            }

            // 使用 tempAdapter 继续操作
            hr = tempAdapter->EnumOutputs(0, &dxgiOutput);
            if (FAILED(hr) || !dxgiOutput) {
                LOG_ERROR("[ScreenCapture] 当前显卡没有挂载显示器 (WinLogon可能会接管), hr=0x%08X", hr);
                return false;
            }

            // 保存到类成员（如果需要的话），或者继续使用 tempAdapter
            dxgiAdapter = tempAdapter;  // 假设类成员是 ComPtr<IDXGIAdapter> 类型

            dxgiOutput.As(&dxgiOutput1);

            hr = dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &dxgiDuplication);
            if (FAILED(hr)) {
                LOG_ERROR("[ScreenCapture] DuplicateOutput 失败, hr=0x%08X", hr);
                handleCaptureError(hr);
                return false;
            }

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
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

            if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &sharedTexture))) return false;

            // 仅在环境支持时创建硬件零拷贝池
            if (canZeroCopy) {
                LOG_INFO("[ScreenCapture] 创建硬件零拷贝纹理池...");
                for (int i = 0; i < YUV_BUFFERS; i++) {
                    if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &hwSharedTextures[i]))) return false;
                    Microsoft::WRL::ComPtr<IDXGIResource> dxgiResource;
                    if (SUCCEEDED(hwSharedTextures[i].As(&dxgiResource))) {
                        dxgiResource->GetSharedHandle(&hwSharedHandles[i]);
                    }
                    else {
                        return false;
                    }
                    hwSharedBusy[i].store(false);
                }
            }

            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            for (int i = 0; i < YUV_BUFFERS; i++) {
                d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextures[i]);
            }

            return true;
        }

        bool ScreenCapture::initializeGPUConverter() {
            // 核心修改：Compute Shader 输出格式变更为 NV12 (UV交错存取)
            // 彻底去除了原子锁(Interlocked)操作，大幅提升了 GPU 并发写入性能
            const char* shaderSource = R"(
                Texture2D<float4> inputTexture : register(t0);
                RWStructuredBuffer<uint> outputBuffer : register(u0);
                cbuffer Constants : register(b0) { uint width; uint height; uint ySize; uint uvSize; }
                
                uint GetY(float4 c) { return ((66 * (uint)(c.r * 255.0f) + 129 * (uint)(c.g * 255.0f) + 25 * (uint)(c.b * 255.0f) + 128) >> 8) + 16; }
                uint GetU(float4 c) { return ((-38 * (uint)(c.r * 255.0f) - 74 * (uint)(c.g * 255.0f) + 112 * (uint)(c.b * 255.0f) + 128) >> 8) + 128; }
                uint GetV(float4 c) { return ((112 * (uint)(c.r * 255.0f) - 94 * (uint)(c.g * 255.0f) - 18 * (uint)(c.b * 255.0f) + 128) >> 8) + 128; }[numthreads(32, 2, 1)]
                void main(uint3 id : SV_DispatchThreadID) {
                    uint startX = id.x * 4;
                    uint y = id.y;
                    if (startX >= width || y >= height) return;
                    
                    // 1. 读取 4 个水平像素
                    float4 c0 = inputTexture.Load(int3(startX + 0, y, 0));
                    float4 c1 = inputTexture.Load(int3(startX + 1, y, 0));
                    float4 c2 = inputTexture.Load(int3(startX + 2, y, 0));
                    float4 c3 = inputTexture.Load(int3(startX + 3, y, 0));
                    
                    // 2. 计算并写入 Y 平面 (4个Y打包成1个uint)
                    uint y0 = GetY(c0); uint y1 = GetY(c1); uint y2 = GetY(c2); uint y3 = GetY(c3);
                    outputBuffer[(y * width + startX) >> 2] = (y3 << 24) | (y2 << 16) | (y1 << 8) | y0;
                    
                    // 3. 计算并写入 UV 平面 (NV12 交错格式)
                    if ((y & 1) == 0 && (y + 1 < height)) {
                        float4 n0 = inputTexture.Load(int3(startX + 0, y + 1, 0));
                        float4 n1 = inputTexture.Load(int3(startX + 1, y + 1, 0));
                        float4 n2 = inputTexture.Load(int3(startX + 2, y + 1, 0));
                        float4 n3 = inputTexture.Load(int3(startX + 3, y + 1, 0));
                        
                        // 2x2 块采样求均值
                        float4 avg0 = (c0 + c1 + n0 + n1) * 0.25f;
                        float4 avg1 = (c2 + c3 + n2 + n3) * 0.25f;
                        
                        uint u0 = GetU(avg0); uint v0 = GetV(avg0);
                        uint u1 = GetU(avg1); uint v1 = GetV(avg1);
                        
                        // NV12 格式：U 和 V 在同一个平面交错 (U0 V0 U1 V1)
                        // 每 4 个横向像素产生 4 字节的 UV 数据，恰好是一个 uint
                        uint uvRow = y >> 1;
                        uint uvPos = ySize + (uvRow * width) + startX; 
                        uint uvIdx = uvPos >> 2; 
                        
                        // 直接安全地覆盖写入，再也不需要 Interlocked 原子锁！
                        outputBuffer[uvIdx] = (v1 << 24) | (u1 << 16) | (v0 << 8) | u0;
                    }
                }
            )";

            Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob, errorBlob;
            HRESULT hr = D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob);
            if (FAILED(hr)) return false;
            d3dDevice->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &yuvComputeShader);

            // NV12 的总尺寸依然是 宽 x 高 x 1.5，和 YUV420 总体积一样
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

            for (int i = 0; i < YUV_BUFFERS; i++) {
                if (FAILED(d3dDevice->CreateBuffer(&bufferDesc, nullptr, &yuvStagingBuffers[i].buffer))) return false;
                yuvStagingBuffers[i].isBusy = false;
                yuvStagingBuffers[i].mappedData = nullptr;
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

        bool ScreenCapture::initializeProcessor() {
            if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&proVideoDevice)))) return false;
            if (FAILED(d3dContext->QueryInterface(IID_PPV_ARGS(&proVideoContext)))) return false;

            UINT alignedWidth = (config.width + 1) & ~1;
            UINT alignedHeight = (config.height + 1) & ~1;

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
            contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            contentDesc.InputWidth = config.width;
            contentDesc.InputHeight = config.height;
            contentDesc.OutputWidth = alignedWidth;
            contentDesc.OutputHeight = alignedHeight;
            contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            if (FAILED(proVideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &proVideoProcessorEnum))) return false;

            UINT flags = 0;
            if (FAILED(proVideoProcessorEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags))) return false;
            if ((flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) return false;

            if (FAILED(proVideoDevice->CreateVideoProcessor(proVideoProcessorEnum.Get(), 0, &proVideoProcessor))) return false;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = alignedWidth;
            texDesc.Height = alignedHeight;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_NV12;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

            if (FAILED(d3dDevice->CreateTexture2D(&texDesc, nullptr, &proOutputTex))) return false;

            texDesc.Usage = D3D11_USAGE_STAGING;
            texDesc.BindFlags = 0;
            texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            texDesc.MiscFlags = 0;

            for (int i = 0; i < YUV_BUFFERS; i++) {
                if (FAILED(d3dDevice->CreateTexture2D(&texDesc, nullptr, &nv12TextureBuffers[i].buffer))) return false;
                nv12TextureBuffers[i].isBusy = false;
                nv12TextureBuffers[i].mappedSubresource.pData = nullptr;
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
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // 1. 硬件零拷贝通道 (NVIDIA)
                if (canZeroCopy && gpuDataHandle) {
                    int lastIdx = (currentHwSharedIdx - 1 + YUV_BUFFERS) % YUV_BUFFERS;
                    if (!hwSharedBusy[lastIdx].load()) { // 确保消费端上一轮已经用完
                        hwSharedBusy[lastIdx].store(true);
                        gpuDataHandle(
                            hwSharedTextures[lastIdx].Get(),
                            hwSharedHandles[lastIdx],
                            config.width, config.height,
                            &hwSharedBusy[lastIdx],
                            config.uselevels
                        );
                    }
                    return true;
                }

                // 2. 软件回调通道
                if (dataHandle) {
                    if (config.uselevels == CaptureLevels::PRO) {
                        int lastIdx = (currentProIdx - 1 + YUV_BUFFERS) % YUV_BUFFERS;
                        Nv12TextureBuffer* targetFrame = &nv12TextureBuffers[lastIdx];
                        // 指针还在，直接发！
                        if (targetFrame->mappedSubresource.pData && !targetFrame->isBusy.load()) {
                            targetFrame->isBusy.store(true);
                            dataHandle(reinterpret_cast<const uint8_t*>(targetFrame->mappedSubresource.pData),
                                config.width, config.height, &targetFrame->isBusy,
                                targetFrame->mappedSubresource.RowPitch, CaptureLevels::PRO);
                        }
                    }
                    else if (config.uselevels == CaptureLevels::GPU && yuvComputeShader) {
                        int lastIdx = (currentYuvIdx - 1 + YUV_BUFFERS) % YUV_BUFFERS;
                        YuvStagingBuffer* targetBuffer = &yuvStagingBuffers[lastIdx];
                        // 指针还在，直接发！
                        if (targetBuffer->mappedData && !targetBuffer->isBusy.load()) {
                            targetBuffer->isBusy.store(true);
                            dataHandle(targetBuffer->mappedData, config.width, config.height,
                                &targetBuffer->isBusy, config.width, CaptureLevels::GPU);
                        }
                    }
                    else {
                        int lastIdx = (currentTexture - 1 + YUV_BUFFERS) % YUV_BUFFERS;
                        D3D11_MAPPED_SUBRESOURCE mapped;
                        if (SUCCEEDED(d3dContext->Map(stagingTextures[lastIdx].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            dataHandle(reinterpret_cast<const uint8_t*>(mapped.pData), config.width, config.height, nullptr, mapped.RowPitch, CaptureLevels::CPU);
                            d3dContext->Unmap(stagingTextures[lastIdx].Get(), 0);
                        }
                    }
                }
                return true;
            }
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
            if (canZeroCopy && gpuDataHandle) {
                int hwIdx = -1;

                // 遍历整个环形队列，寻找第一个真正空闲的槽位
                for (int i = 0; i < YUV_BUFFERS; i++) {
                    int checkIdx = (currentHwSharedIdx + i) % YUV_BUFFERS;
                    if (!hwSharedBusy[checkIdx].load()) {
                        hwIdx = checkIdx;
                        // 找到后，将全局游标推到下一个位置，为下一帧做准备
                        currentHwSharedIdx = (checkIdx + 1) % YUV_BUFFERS;
                        break;
                    }
                }

                // 如果 24 个槽位全都被 WebRTC 编码线程卡住了，这才无奈丢帧
                if (hwIdx == -1) {
                    LOG_WARNING("[ScreenCapture] 警告：24个硬件零拷贝槽位全满，主动丢弃当前帧！");
                    return true;
                }

                Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;
                if (SUCCEEDED(hwSharedTextures[hwIdx].As(&km))) {
                    if (km->AcquireSync(0, INFINITE) == S_OK) {
                        d3dContext->CopyResource(hwSharedTextures[hwIdx].Get(), texture);

                        d3dContext->Flush();
                        km->ReleaseSync(0); // 维持 0 不变

                        hwSharedBusy[hwIdx].store(true);
                        gpuDataHandle(
                            hwSharedTextures[hwIdx].Get(),
                            hwSharedHandles[hwIdx],
                            config.width, config.height,
                            &hwSharedBusy[hwIdx],
                            config.uselevels
                        );
                    }
                    else {
                        LOG_ERROR("[ScreenCapture] AcquireSync 失败，槽位 %d 锁定异常", hwIdx);
                    }
                }
                return true;
            }

            if (dataHandle) {
                if (config.uselevels == CaptureLevels::PRO) return processFramePro(texture);
                else if (config.uselevels == CaptureLevels::GPU && yuvComputeShader) return processFrameGPU(texture);
                return processFrameCPU(texture);
            }
            return true;
        }

        bool ScreenCapture::processFrameCPU(ID3D11Texture2D* texture) {
            if (!texture || !d3dContext) return false;
            d3dContext->CopyResource(stagingTextures[currentTexture].Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(d3dContext->Map(stagingTextures[currentTexture].Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

            if (dataHandle) {
                dataHandle(reinterpret_cast<const uint8_t*>(mapped.pData), config.width, config.height, nullptr, mapped.RowPitch, CaptureLevels::CPU);
            }
            d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);
            currentTexture = (currentTexture + 1) % YUV_BUFFERS;
            return true;
        }

        bool ScreenCapture::processFrameGPU(ID3D11Texture2D* texture) {
            if (!texture || !d3dContext || !yuvComputeShader) return false;

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

            YuvStagingBuffer* targetBuffer = &yuvStagingBuffers[currentYuvIdx];

            if (targetBuffer->isBusy.load()) {
                int retries = 0;
                while (targetBuffer->isBusy.load() && retries < YUV_BUFFERS) {
                    currentYuvIdx = (currentYuvIdx + 1) % YUV_BUFFERS;
                    targetBuffer = &yuvStagingBuffers[currentYuvIdx];
                    retries++;
                }
            }

            if (targetBuffer->mappedData) {
                d3dContext->Unmap(targetBuffer->buffer.Get(), 0);
                targetBuffer->mappedData = nullptr;
            }

            d3dContext->CopyResource(targetBuffer->buffer.Get(), yuvOutputBuffer.Get());

            if (SUCCEEDED(d3dContext->Map(targetBuffer->buffer.Get(), 0, D3D11_MAP_READ, 0, &targetBuffer->mappedSubresource))) {
                targetBuffer->mappedData = static_cast<uint8_t*>(targetBuffer->mappedSubresource.pData);
                targetBuffer->isBusy.store(true);

                if (dataHandle) {
                    dataHandle(targetBuffer->mappedData, config.width, config.height, &targetBuffer->isBusy, config.width, CaptureLevels::GPU);
                }
                else {
                    d3dContext->Unmap(targetBuffer->buffer.Get(), 0);
                    targetBuffer->mappedData = nullptr;
                    targetBuffer->isBusy.store(false);
                }
            }

            currentYuvIdx = (currentYuvIdx + 1) % YUV_BUFFERS;
            return true;
        }

        bool ScreenCapture::processFramePro(ID3D11Texture2D* texture) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            inputDesc.Texture2D.MipSlice = 0;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
            if (FAILED(proVideoDevice->CreateVideoProcessorInputView(texture, proVideoProcessorEnum.Get(), &inputDesc, &inputView))) return false;

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
            if (FAILED(proVideoDevice->CreateVideoProcessorOutputView(proOutputTex.Get(), proVideoProcessorEnum.Get(), &outputDesc, &outputView))) return false;

            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView.Get();
            HRESULT hr = proVideoContext->VideoProcessorBlt(proVideoProcessor.Get(), outputView.Get(), 0, 1, &stream);
            if (FAILED(hr)) return false;

            Nv12TextureBuffer* targetFrame = &nv12TextureBuffers[currentProIdx];

            if (targetFrame->isBusy.load()) {
                int retries = 0;
                while (targetFrame->isBusy.load() && retries < YUV_BUFFERS) {
                    currentProIdx = (currentProIdx + 1) % YUV_BUFFERS;
                    targetFrame = &nv12TextureBuffers[currentProIdx];
                    retries++;
                }
            }

            if (targetFrame->mappedSubresource.pData) {
                d3dContext->Unmap(targetFrame->buffer.Get(), 0);
                targetFrame->mappedSubresource.pData = nullptr;
            }

            d3dContext->CopyResource(targetFrame->buffer.Get(), proOutputTex.Get());

            if (SUCCEEDED(d3dContext->Map(targetFrame->buffer.Get(), 0, D3D11_MAP_READ, 0, &targetFrame->mappedSubresource))) {
                targetFrame->isBusy.store(true);

                if (dataHandle) {
                    dataHandle(reinterpret_cast<const uint8_t*>(targetFrame->mappedSubresource.pData),
                        config.width, config.height, &targetFrame->isBusy, targetFrame->mappedSubresource.RowPitch, CaptureLevels::PRO);
                }
                else {
                    d3dContext->Unmap(targetFrame->buffer.Get(), 0);
                    targetFrame->mappedSubresource.pData = nullptr;
                    targetFrame->isBusy.store(false);
                }
            }
            else {
                d3dContext->Unmap(targetFrame->buffer.Get(), 0);
                targetFrame->mappedSubresource.pData = nullptr;
                targetFrame->isBusy.store(false);
            }

            currentProIdx = (currentProIdx + 1) % YUV_BUFFERS;
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
            dxgiDuplication.Reset();

            for (auto& st : stagingTextures) st.Reset();
            sharedTexture.Reset();
            sharedHandle = nullptr;

            for (int i = 0; i < YUV_BUFFERS; i++) {
                hwSharedTextures[i].Reset();
                hwSharedHandles[i] = nullptr;
                hwSharedBusy[i].store(false);
            }

            yuvComputeShader.Reset();
            yuvOutputBuffer.Reset();
            yuvConstantBuffer.Reset();
            yuvUAV.Reset();

            proVideoDevice.Reset();
            proVideoContext.Reset();
            proVideoProcessor.Reset();
            proVideoProcessorEnum.Reset();
            proOutputTex.Reset();

            for (int i = 0; i < YUV_BUFFERS; i++) {
                yuvStagingBuffers[i].buffer.Reset();
                yuvStagingBuffers[i].mappedData = nullptr;
                yuvStagingBuffers[i].isBusy = false;

                nv12TextureBuffers[i].buffer.Reset();
                nv12TextureBuffers[i].isBusy = false;
                nv12TextureBuffers[i].mappedSubresource.pData = nullptr;
            }
        }
    }
}