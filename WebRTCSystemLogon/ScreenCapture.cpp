#include "ScreenCapture.h"
#include <algorithm>
#include <thread>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include "Logger.h"
#include "Utils.h"

struct DirtyRegionTracker {
    std::vector<RECT> dirtyRects;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moveRects;
    std::vector<uint8_t> metadataBuffer;
    bool hasUpdates = false;
    bool fullFrameRequired = true;

    DirtyRegionTracker() {
        metadataBuffer.reserve(32768);
        dirtyRects.reserve(64);
        moveRects.reserve(32);
    }

    void Reset() {
        dirtyRects.clear();
        moveRects.clear();
        hasUpdates = false;
    }
};


struct GPUTextureRing {
    static constexpr int RING_SIZE = 6;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[RING_SIZE];
    Microsoft::WRL::ComPtr<ID3D11Query> queries[RING_SIZE];
    std::atomic<int> writeIndex{ 0 };
    std::atomic<int> readIndex{ 0 };
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    bool Initialize(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext, int width, int height) {
        device = d3dDevice;
        context = d3dContext;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;

        for (int i = 0; i < RING_SIZE; i++) {
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &textures[i]);
            if (FAILED(hr)) return false;

            hr = device->CreateQuery(&queryDesc, &queries[i]);
            if (FAILED(hr)) return false;
        }
        return true;
    }

    ID3D11Texture2D* GetWriteTexture() {
        return textures[writeIndex % RING_SIZE].Get();
    }

    void MarkWriteComplete() {
        int idx = writeIndex % RING_SIZE;
        context->End(queries[idx].Get());
        writeIndex++;
    }

    ID3D11Texture2D* TryGetReadTexture() {
        int idx = readIndex % RING_SIZE;
        BOOL dataAvailable = FALSE;
        HRESULT hr = context->GetData(queries[idx].Get(), &dataAvailable, sizeof(BOOL), D3D11_ASYNC_GETDATA_DONOTFLUSH);

        if (SUCCEEDED(hr) && dataAvailable) {
            readIndex++;
            return textures[idx].Get();
        }
        return nullptr;
    }
};

ScreenCapture::ScreenCapture()
    : encoderChannel(encoderContext, 128) {

    encoderWorkGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(encoderContext));

    winLogonSwitcher = std::make_unique<WinLogon>();
    winLogonSwitcher->SwitchToDefaultDesktop();

    dirtyTracker = std::make_unique<DirtyRegionTracker>();
    gpuRing = std::make_unique<GPUTextureRing>();

}

ScreenCapture::~ScreenCapture() {
    stopCapture();

    if (isOnWinLogonDesktop && winLogonSwitcher) {
        winLogonSwitcher->SwitchToDefaultDesktop();
    }

    releaseResources();
}

bool ScreenCapture::initialize() {
    Logger::getInstance()->info("=== Starting DXGI ScreenCapture initialization ===");

    if (!frameCallback && !gpuEncoderCallback) {
        Logger::getInstance()->warning("No callback set! Call setFrameCallback() or setGPUEncoderCallback()!");
    }

    try {
        if (!initializeDXGI()) {
            Logger::getInstance()->error("Failed to initialize DXGI");
            winLogonSwitcher->SwitchToWinLogonDesktop();
            desktopSwitchInProgress = true;
            if (!initializeDXGI()) {
                Logger::getInstance()->error("Failed to initialize DXGI on WinLogon desktop");
                return false;
            }
        }
        Logger::getInstance()->info("DXGI initialized successfully");

        if (!initializeGPUConverter()) {
            Logger::getInstance()->warning("GPU YUV converter initialization failed, will use CPU");
        }

        const int yuvSize = config.width * config.height * 3 / 2;
        yuvBuffer.resize(yuvSize);
        Logger::getInstance()->info("YUV buffer allocated: " + std::to_string(yuvSize) + " bytes");

        initializeFramePool(20);

        frameInterval = std::chrono::milliseconds(1000 / config.fps);
        lastFrameTime = std::chrono::steady_clock::now();

        Logger::getInstance()->info("Frame rate set to " + std::to_string(config.fps) + " fps");
        Logger::getInstance()->info("Dirty Rectangles: " + std::string(config.enableDirtyRects ? "Enabled" : "Disabled"));

        Logger::getInstance()->info("=== DXGI ScreenCapture initialization completed ===");
        return true;
    }
    catch (const std::exception& ex) {
        Logger::getInstance()->error("Exception during initialization: " + std::string(ex.what()));
        return false;
    }
}

void ScreenCapture::initializeFramePool(size_t poolSize) {
    for (size_t i = 0; i < poolSize; ++i) {
        auto frame = std::make_shared<CapturedFrame>(config.width, config.height, config.width * 4);
        framePool.enqueue(frame);
    }
    Logger::getInstance()->info("Frame pool initialized with " + std::to_string(poolSize) + " frames");
}

std::shared_ptr<CapturedFrame> ScreenCapture::getFrameFromPool() {
    std::shared_ptr<CapturedFrame> frame;
    if (!framePool.try_dequeue(frame)) {
        frame = std::make_shared<CapturedFrame>(config.width, config.height, config.width * 4);
    }
    return frame;
}

inline void ScreenCapture::returnFrameToPool(std::shared_ptr<CapturedFrame> frame) {
    if (frame && framePool.size_approx() < 30) {
        framePool.enqueue(frame);
    }
}

bool ScreenCapture::initializeDXGI() {
    HRESULT hr;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    createFlags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;


#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &d3dDevice,
        &featureLevel,
        &d3dContext
    );

    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create D3D11 device: 0x" +
            std::to_string(static_cast<unsigned int>(hr)));
        return false;
    }

    hr = d3dDevice.As(&d3dDevice1);
    if (SUCCEEDED(hr)) {
        hr = d3dContext.As(&d3dContext1);
        useAdvancedFeatures = SUCCEEDED(hr);
        if (useAdvancedFeatures) {
            Logger::getInstance()->debug("D3D11.1 features available");
        }
    }

    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to get DXGI device");
        return false;
    }

    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to get DXGI adapter");
        return false;
    }

    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to enumerate outputs");
        return false;
    }

    hr = dxgiOutput.As(&dxgiOutput1);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to get IDXGIOutput1");
        return false;
    }

    hr = dxgiOutput.As(&dxgiOutput5);
    if (SUCCEEDED(hr)) {
        Logger::getInstance()->debug("DXGI 1.5 features available");
    }

    static bool init = false;
    hr = dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &dxgiDuplication);

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            Logger::getInstance()->error("Desktop duplication not available (may be in use by another process)");
        }
        else if (hr == E_ACCESSDENIED) {
            if (init == true) return false;
            init = true;

            Logger::getInstance()->error("Access denied for desktop duplication");

            if (!desktopSwitchInProgress) {
                this->releaseResourceDXGI();
                winLogonSwitcher->SwitchToWinLogonDesktop();
                this->initializeDXGI();
                this->initializeGPUConverter();
                desktopSwitchInProgress = true;
            }
            else {
                this->releaseResourceDXGI();
                winLogonSwitcher->SwitchToDefaultDesktop();
                this->initializeDXGI();
                this->initializeGPUConverter();
                desktopSwitchInProgress = false;
            }
        }
        else {
            Logger::getInstance()->error("Failed to duplicate output: 0x" +
                std::to_string(static_cast<unsigned int>(hr)));
        }
        return false;
    }

    init = false;

    DXGI_OUTDUPL_DESC duplDesc;
    dxgiDuplication->GetDesc(&duplDesc);

    config.width = duplDesc.ModeDesc.Width;
    config.height = duplDesc.ModeDesc.Height;

    Logger::getInstance()->info("Desktop dimensions: " + std::to_string(config.width) +
        "x" + std::to_string(config.height));

    if (!gpuRing->Initialize(d3dDevice.Get(), d3dContext.Get(), config.width, config.height)) {
        Logger::getInstance()->error("Failed to initialize GPU ring buffer");
        return false;
    }

    // 创建工作纹理用于脏矩形处理
    D3D11_TEXTURE2D_DESC workingDesc = {};
    workingDesc.Width = config.width;
    workingDesc.Height = config.height;
    workingDesc.MipLevels = 1;
    workingDesc.ArraySize = 1;
    workingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    workingDesc.SampleDesc.Count = 1;
    workingDesc.Usage = D3D11_USAGE_DEFAULT;
    workingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = d3dDevice->CreateTexture2D(&workingDesc, nullptr, &workingTexture);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create working texture");
        return false;
    }

    // 创建staging textures
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = config.width;
    desc.Height = config.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextures[i]);
        if (FAILED(hr)) {
            Logger::getInstance()->error("Failed to create staging texture " + std::to_string(i));
            return false;
        }
    }

    return true;
}

bool ScreenCapture::initializeGPUConverter() {
    const char* shaderSource = R"(
        Texture2D<float4> inputTexture : register(t0);
        RWStructuredBuffer<uint> outputBuffer : register(u0);
        
        cbuffer Constants : register(b0) {
            uint width;
            uint height;
            uint ySize;
            uint uvSize;
        }
        
        [numthreads(16, 16, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            if (id.x >= width || id.y >= height) return;
            
            float4 color = inputTexture.Load(int3(id.x, id.y, 0));
            
            float b = color.b * 255.0f;
            float g = color.g * 255.0f;
            float r = color.r * 255.0f;
            
            uint y = (uint)((66 * r + 129 * g + 25 * b) / 256 + 16);
            y = min(y, 255u);
            
            uint yIndex = id.y * width + id.x;
            uint arrayIndex = yIndex / 4;
            uint byteOffset = yIndex % 4;
            uint shiftAmount = byteOffset * 8;
            uint mask = 0xFF << shiftAmount;
            uint value = y << shiftAmount;
            
            InterlockedAnd(outputBuffer[arrayIndex], ~mask);
            InterlockedOr(outputBuffer[arrayIndex], value);
            
            if ((id.x % 2 == 0) && (id.y % 2 == 0)) {
                float4 c00 = color;
                float4 c10 = inputTexture.Load(int3(min(id.x + 1, width - 1), id.y, 0));
                float4 c01 = inputTexture.Load(int3(id.x, min(id.y + 1, height - 1), 0));
                float4 c11 = inputTexture.Load(int3(min(id.x + 1, width - 1), min(id.y + 1, height - 1), 0));
                
                float4 avg = (c00 + c10 + c01 + c11) * 0.25f * 255.0f;
                float avgB = avg.b;
                float avgG = avg.g;
                float avgR = avg.r;
                
                uint u = (uint)((-38 * avgR - 74 * avgG + 112 * avgB) / 256 + 128);
                uint v = (uint)((112 * avgR - 94 * avgG - 18 * avgB) / 256 + 128);
                u = clamp(u, 0u, 255u);
                v = clamp(v, 0u, 255u);
                
                uint uvIndex = (id.y / 2) * (width / 2) + (id.x / 2);
                
                uint uPos = ySize + uvIndex;
                uint uArrayIndex = uPos / 4;
                uint uByteOffset = uPos % 4;
                uint uShift = uByteOffset * 8;
                
                InterlockedAnd(outputBuffer[uArrayIndex], ~(0xFF << uShift));
                InterlockedOr(outputBuffer[uArrayIndex], u << uShift);
                
                uint vPos = ySize + uvSize + uvIndex;
                uint vArrayIndex = vPos / 4;
                uint vByteOffset = vPos % 4;
                uint vShift = vByteOffset * 8;
                
                InterlockedAnd(outputBuffer[vArrayIndex], ~(0xFF << vShift));
                InterlockedOr(outputBuffer[vArrayIndex], v << vShift);
            }
        }
    )";

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
        &shaderBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            Logger::getInstance()->error("Shader compilation error: " +
                std::string((char*)errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = d3dDevice->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &yuvComputeShader
    );

    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create compute shader");
        return false;
    }

    const UINT yuvBufferSize = config.width * config.height * 3 / 2;
    const UINT bufferSizeInUints = (yuvBufferSize + 3) / 4;

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = bufferSizeInUints * sizeof(UINT);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(UINT);

    std::vector<UINT> initialData(bufferSizeInUints, 0);
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = initialData.data();

    hr = d3dDevice->CreateBuffer(&bufferDesc, &initData, &yuvOutputBuffer);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create output buffer");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = bufferSizeInUints;

    hr = d3dDevice->CreateUnorderedAccessView(yuvOutputBuffer.Get(), &uavDesc, &yuvUAV);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create UAV");
        return false;
    }

    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.BindFlags = 0;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufferDesc.MiscFlags = 0;

    hr = d3dDevice->CreateBuffer(&bufferDesc, nullptr, &stagingBuffer);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create staging buffer");
        return false;
    }

    return true;
}

bool ScreenCapture::startCapture() {
    if (capturing.load()) {
        Logger::getInstance()->warning("Capture already started");
        return true;
    }

    capturing = true;
    encoderRunning = true;

    encoderContext.restart();

    encoderThread = std::thread([this]() {
        Logger::getInstance()->info("Encoder thread started");

        try {
            boost::asio::co_spawn(encoderContext, [this]() -> boost::asio::awaitable<void> {
                while (encoderRunning.load()) {
                    try {
                        auto [ec] = co_await encoderChannel.async_receive(
                            boost::asio::as_tuple(boost::asio::use_awaitable));

                        if (ec) {
                            if (ec == boost::asio::error::operation_aborted) {
                                Logger::getInstance()->info("Encoder channel closed, stopping");
                                break;
                            }
                        }

                        std::shared_ptr<CapturedFrame> frame;
                        int processedCount = 0;
                        int maxBatch = 10;

                        while (frameQueue.try_dequeue(frame) && processedCount < maxBatch) {
                            encodeFrame(frame);
                            returnFrameToPool(frame);
                            processedCount++;
                        }

                    }
                    catch (const std::exception& e) {
                        Logger::getInstance()->error("Error in encoder coroutine: " + std::string(e.what()));
                    }
                }
                co_return;
                }, boost::asio::detached);

            size_t handlers = encoderContext.run();
            Logger::getInstance()->info("io_context.run() returned after processing " +
                std::to_string(handlers) + " handlers");
        }
        catch (const std::exception& e) {
            Logger::getInstance()->error("Fatal error in encoder thread: " + std::string(e.what()));
        }

        Logger::getInstance()->info("Encoder thread ending");
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    captureThread = std::thread([this]() {
        Logger::getInstance()->info("Capture thread starting");
        captureThreadFunc();
        Logger::getInstance()->info("Capture thread ending");
        });

    Logger::getInstance()->info("Capture started successfully");
    return true;
}

void ScreenCapture::captureThreadFunc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(GetCurrentThread(), 0x0F);

    while (capturing.load()) {

        bool success = captureFrame();

        if (!success) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
    }
}

bool ScreenCapture::captureFrame() {
    if (!dxgiDuplication) {
        invalidCallDxgi++;

        if (!desktopSwitchInProgress && invalidCallDxgi == 2) {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToWinLogonDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = true;
        }
        else {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToDefaultDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = false;
        }
        return false;
    }

    invalidCallDxgi = 0;

    HRESULT hr;
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // 使用较短的超时时间 - 对于60fps，使用16ms
    hr = dxgiDuplication->AcquireNextFrame(16, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;  // 没有新帧，正常返回
    }

    if (FAILED(hr)) {
        handleCaptureError(hr);
        return false;
    }

    // RAII包装器，确保帧总是被释放
    struct FrameReleaser {
        IDXGIOutputDuplication* duplication;
        bool shouldRelease;
        ~FrameReleaser() {
            if (shouldRelease && duplication) {
                duplication->ReleaseFrame();
            }
        }
    } frameReleaser{ dxgiDuplication.Get(), true };

    invalidCallCount = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = desktopResource.As(&acquiredTexture);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to get texture from desktop resource");
        return false;
    }

    // 修复：处理脏矩形逻辑
    bool needsFullFrame = false;

    if (config.enableDirtyRects) {
        // 启用脏矩形时的处理逻辑
        needsFullFrame = dirtyTracker->fullFrameRequired;
        if (frameInfo.TotalMetadataBufferSize > 0) {
            ProcessDirtyRects(&frameInfo, acquiredTexture.Get(), workingTexture.Get());
            needsFullFrame = dirtyTracker->fullFrameRequired;
        }

        if (needsFullFrame) {
            d3dContext->CopyResource(workingTexture.Get(), acquiredTexture.Get());
            dirtyTracker->fullFrameRequired = false;
        }
    }
    else {
        // 禁用脏矩形时，始终复制完整帧
        d3dContext->CopyResource(workingTexture.Get(), acquiredTexture.Get());
        needsFullFrame = true;  // 标记已处理完整帧
    }

    // 立即处理帧内容 - 在释放前完成所有必要的复制
    bool result = processFrame(workingTexture.Get());

    // frameReleaser析构时会自动释放帧
    return result;
}

// 优化的processFrame函数，使用异步复制
bool ScreenCapture::processFrame(ID3D11Texture2D* texture) {
    if (!texture || !d3dContext || !stagingTextures[currentTexture]) {
        Logger::getInstance()->error("Invalid texture or D3D context");
        return false;
    }

    // GPU编码路径
    if (config.enableGPUEncoding && gpuEncoderCallback) {
        // 使用GPU ring buffer
        if (gpuRing) {
            ID3D11Texture2D* ringTexture = gpuRing->GetWriteTexture();
            if (ringTexture) {
                d3dContext->CopyResource(ringTexture, texture);
                gpuRing->MarkWriteComplete();

                // 异步检查是否有完成的纹理
                ID3D11Texture2D* readyTexture = gpuRing->TryGetReadTexture();
                if (readyTexture) {
                    gpuEncoderCallback(readyTexture);
                }
                return true;
            }
        }

        // 回退到直接调用
        ProcessOnGPU(texture);
        return true;
    }

    // GPU YUV转换路径
    if (yuvComputeShader) {
        std::vector<uint8_t> tempYuvBuffer;
        if (convertBGRAToYUV420_GPU(texture, tempYuvBuffer)) {
            auto frame = getFrameFromPool();
            if (!frame) {
                return false;
            }

            frame->bgraData = std::move(tempYuvBuffer);
            frame->width = config.width;
            frame->height = config.height;
            frame->stride = config.width;
            frame->isYUV = true;
            frame->timestamp = std::chrono::steady_clock::now();

            frameQueue.enqueue(frame);

            if (encoderChannel.is_open()) {
                boost::system::error_code ec;
                encoderChannel.try_send(ec);
            }

            return true;
        }
    }

    // CPU路径 - 使用异步Map
    d3dContext->CopyResource(stagingTextures[currentTexture].Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = d3dContext->Map(
        stagingTextures[currentTexture].Get(),
        0,
        D3D11_MAP_READ,
        0,  // 非阻塞
        &mapped);

    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to map texture: 0x" +
            std::to_string(static_cast<unsigned int>(hr)));
        return false;
    }

    auto frame = getFrameFromPool();
    if (!frame) {
        d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);
        return false;
    }

    const uint8_t* srcData = static_cast<const uint8_t*>(mapped.pData);
    const size_t bytesPerRow = config.width * 4;
    uint8_t* dstData = frame->bgraData.data();

    if (mapped.RowPitch == bytesPerRow) {
        fastCopy(dstData, srcData, bytesPerRow * config.height);
    }
    else {
        for (int row = 0; row < config.height; ++row) {
            fastCopy(dstData + row * bytesPerRow,
                srcData + row * mapped.RowPitch,
                bytesPerRow);
        }
    }

    d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);

    frame->width = config.width;
    frame->height = config.height;
    frame->stride = bytesPerRow;
    frame->timestamp = std::chrono::steady_clock::now();

    size_t queueSize = frameQueue.size_approx();
    if (queueSize >= MAX_QUEUE_SIZE) {
        std::shared_ptr<CapturedFrame> oldFrame;
        if (frameQueue.try_dequeue(oldFrame)) {
            returnFrameToPool(oldFrame);
        }
    }

    frameQueue.enqueue(frame);

    if (encoderChannel.is_open()) {
        boost::system::error_code ec;
        encoderChannel.try_send(ec);
    }

    currentTexture = (currentTexture + 1) % NUM_BUFFERS;

    return true;
}


void ScreenCapture::ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture) {
    dirtyTracker->Reset();

    if (frameInfo->TotalMetadataBufferSize == 0) {
        dirtyTracker->fullFrameRequired = true;
        return;
    }

    if (dirtyTracker->metadataBuffer.size() < frameInfo->TotalMetadataBufferSize) {
        dirtyTracker->metadataBuffer.resize(frameInfo->TotalMetadataBufferSize);
    }

    UINT bufSize = frameInfo->TotalMetadataBufferSize;

    // 获取移动矩形
    DXGI_OUTDUPL_MOVE_RECT* moveRects = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(
        dirtyTracker->metadataBuffer.data());
    UINT moveRectSize = bufSize;

    HRESULT hr = dxgiDuplication->GetFrameMoveRects(moveRectSize, moveRects, &moveRectSize);
    if (SUCCEEDED(hr) && moveRectSize > 0) {
        UINT moveCount = moveRectSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);

        for (UINT i = 0; i < moveCount; i++) {
            ProcessMoveRect(sourceTexture, &moveRects[i], destTexture);
        }
    }

    // 获取脏矩形
    RECT* dirtyRects = reinterpret_cast<RECT*>(dirtyTracker->metadataBuffer.data());
    UINT dirtyRectSize = bufSize;

    hr = dxgiDuplication->GetFrameDirtyRects(dirtyRectSize, dirtyRects, &dirtyRectSize);
    if (SUCCEEDED(hr) && dirtyRectSize > 0) {
        UINT dirtyCount = dirtyRectSize / sizeof(RECT);

        std::vector<RECT> mergedRects = MergeDirtyRects(dirtyRects, dirtyCount);

        for (const auto& rect : mergedRects) {
            D3D11_BOX box;
            box.left = rect.left;
            box.top = rect.top;
            box.right = rect.right;
            box.bottom = rect.bottom;
            box.front = 0;
            box.back = 1;

            d3dContext->CopySubresourceRegion(
                destTexture, 0,
                rect.left, rect.top, 0,
                sourceTexture, 0, &box
            );
        }

        dirtyTracker->hasUpdates = true;
    }
}

void ScreenCapture::ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture) {
    D3D11_BOX srcBox;
    srcBox.left = moveRect->SourcePoint.x;
    srcBox.top = moveRect->SourcePoint.y;
    srcBox.right = moveRect->SourcePoint.x + (moveRect->DestinationRect.right - moveRect->DestinationRect.left);
    srcBox.bottom = moveRect->SourcePoint.y + (moveRect->DestinationRect.bottom - moveRect->DestinationRect.top);
    srcBox.front = 0;
    srcBox.back = 1;

    d3dContext->CopySubresourceRegion(
        destTexture, 0,
        moveRect->DestinationRect.left,
        moveRect->DestinationRect.top,
        0,
        sourceTexture, 0,
        &srcBox
    );
}

std::vector<RECT> ScreenCapture::MergeDirtyRects(RECT* rects, UINT count) {
    if (count == 0) return {};
    if (count == 1) return { rects[0] };

    std::vector<RECT> merged;
    merged.reserve(count);

    if (count <= 3) {
        int totalArea = 0;
        for (UINT i = 0; i < count; i++) {
            merged.push_back(rects[i]);
            totalArea += (rects[i].right - rects[i].left) * (rects[i].bottom - rects[i].top);
        }

        if (totalArea > (config.width * config.height * 0.4)) {
            dirtyTracker->fullFrameRequired = true;
            return {};
        }
        return merged;
    }

    std::vector<bool> used(count, false);

    for (UINT i = 0; i < count; i++) {
        if (used[i]) continue;

        RECT current = rects[i];
        used[i] = true;

        bool changed = true;
        while (changed) {
            changed = false;

            for (UINT j = 0; j < count; j++) {
                if (used[j] || j == i) continue;

                const RECT& other = rects[j];

                const int gap = 32;
                if (!(current.right + gap < other.left ||
                    other.right + gap < current.left ||
                    current.bottom + gap < other.top ||
                    other.bottom + gap < current.top)) {

                    RECT mergedRect = {
                        std::min(current.left, other.left),
                        std::min(current.top, other.top),
                        std::max(current.right, other.right),
                        std::max(current.bottom, other.bottom)
                    };

                    int currentArea = (current.right - current.left) * (current.bottom - current.top);
                    int otherArea = (other.right - other.left) * (other.bottom - other.top);
                    int mergedArea = (mergedRect.right - mergedRect.left) * (mergedRect.bottom - mergedRect.top);

                    if (mergedArea <= (currentArea + otherArea) * 1.3f) {
                        current = mergedRect;
                        used[j] = true;
                        changed = true;
                    }
                }
            }
        }

        int area = (current.right - current.left) * (current.bottom - current.top);
        if (area >= 4096) {
            merged.push_back(current);
        }
    }

    int totalArea = 0;
    for (const auto& rect : merged) {
        totalArea += (rect.right - rect.left) * (rect.bottom - rect.top);
    }

    if (totalArea > (config.width * config.height * 0.4) || merged.size() > 32) {
        dirtyTracker->fullFrameRequired = true;
        return {};
    }

    return merged;
}

void ScreenCapture::ProcessOnGPU(ID3D11Texture2D* sourceTexture) {
    if (!gpuEncoderCallback || !sourceTexture) return;
    gpuEncoderCallback(sourceTexture);
}

bool ScreenCapture::convertBGRAToYUV420_GPU(ID3D11Texture2D* sourceTexture, std::vector<uint8_t>& yuvBuffer) {
    if (!yuvComputeShader || !sourceTexture) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSRV;
    HRESULT hr = d3dDevice->CreateShaderResourceView(sourceTexture, &srvDesc, &srcSRV);
    if (FAILED(hr)) {
        return false;
    }

    struct Constants {
        UINT width;
        UINT height;
        UINT ySize;
        UINT uvSize;
    } constants;

    constants.width = config.width;
    constants.height = config.height;
    constants.ySize = config.width * config.height;
    constants.uvSize = constants.ySize / 4;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(Constants);
    cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA cbInitData = {};
    cbInitData.pSysMem = &constants;

    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer;
    hr = d3dDevice->CreateBuffer(&cbDesc, &cbInitData, &constantBuffer);
    if (FAILED(hr)) {
        return false;
    }

    const UINT bufferSizeInUints = (constants.ySize * 3 / 2 + 3) / 4;
    std::vector<UINT> clearData(bufferSizeInUints, 0);
    d3dContext->UpdateSubresource(yuvOutputBuffer.Get(), 0, nullptr, clearData.data(), 0, 0);

    d3dContext->CSSetShader(yuvComputeShader.Get(), nullptr, 0);
    d3dContext->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
    d3dContext->CSSetUnorderedAccessViews(0, 1, yuvUAV.GetAddressOf(), nullptr);
    d3dContext->CSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());

    UINT dispatchX = (config.width + 15) / 16;
    UINT dispatchY = (config.height + 15) / 16;
    d3dContext->Dispatch(dispatchX, dispatchY, 1);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer* nullCB = nullptr;
    d3dContext->CSSetShaderResources(0, 1, &nullSRV);
    d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    d3dContext->CSSetConstantBuffers(0, 1, &nullCB);

    d3dContext->CopyResource(stagingBuffer.Get(), yuvOutputBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = d3dContext->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    const int yuvSize = config.width * config.height * 3 / 2;
    if (yuvBuffer.size() != yuvSize) {
        yuvBuffer.resize(yuvSize);
    }

    fastCopy(yuvBuffer.data(), mapped.pData, yuvSize);
    d3dContext->Unmap(stagingBuffer.Get(), 0);

    return true;
}

void ScreenCapture::encodeFrame(std::shared_ptr<CapturedFrame> frame) {
    if (!frame) {
        Logger::getInstance()->error("encodeFrame: frame is null");
        return;
    }

    if (frame->isYUV || frame->stride == config.width) {
        if (frameCallback) {
            frameCallback(frame->bgraData.data(), frame->bgraData.size(),
                frame->width, frame->height);
        }
    }
    else {
        if (!convertBGRAToYUV420(frame->bgraData.data(), frame->stride, yuvBuffer)) {
            Logger::getInstance()->error("Failed to convert BGRA to YUV420");
            return;
        }

        if (frameCallback) {
            frameCallback(yuvBuffer.data(), yuvBuffer.size(),
                frame->width, frame->height);
        }
    }
}

bool ScreenCapture::convertBGRAToYUV420(const uint8_t* bgraData, int stride,
    std::vector<uint8_t>& yuvBuffer) {

    const int ySize = config.width * config.height;
    const int uvSize = ySize / 4;

    if (yuvBuffer.size() != ySize + 2 * uvSize) {
        yuvBuffer.resize(ySize + 2 * uvSize);
    }

    uint8_t* y = yuvBuffer.data();
    uint8_t* u = y + ySize;
    uint8_t* v = u + uvSize;

    static bool tablesReady = false;
    static int yTable_r[256], yTable_g[256], yTable_b[256];
    static int uTable_r[256], uTable_g[256], uTable_b[256];
    static int vTable_r[256], vTable_g[256], vTable_b[256];

    if (!tablesReady) {
        for (int i = 0; i < 256; i++) {
            yTable_r[i] = (66 * i) >> 8;
            yTable_g[i] = (129 * i) >> 8;
            yTable_b[i] = (25 * i) >> 8;

            uTable_r[i] = (-38 * i) >> 8;
            uTable_g[i] = (-74 * i) >> 8;
            uTable_b[i] = (112 * i) >> 8;

            vTable_r[i] = (112 * i) >> 8;
            vTable_g[i] = (-94 * i) >> 8;
            vTable_b[i] = (-18 * i) >> 8;
        }
        tablesReady = true;
    }

    for (int row = 0; row < config.height; row++) {
        const uint8_t* bgraRow = bgraData + row * stride;
        uint8_t* yRow = y + row * config.width;

        for (int col = 0; col < config.width; col++) {
            const uint8_t* pixel = bgraRow + col * 4;
            yRow[col] = std::min(255, std::max(0,
                yTable_r[pixel[2]] + yTable_g[pixel[1]] + yTable_b[pixel[0]] + 16));
        }
    }

    for (int row = 0; row < config.height; row += 2) {
        const uint8_t* bgraRow = bgraData + row * stride;
        const int uvRow = row >> 1;
        const int halfWidth = config.width >> 1;

        for (int col = 0; col < config.width; col += 2) {
            const uint8_t* pixel = bgraRow + col * 4;
            const int uvIndex = uvRow * halfWidth + (col >> 1);

            int sumB = pixel[0];
            int sumG = pixel[1];
            int sumR = pixel[2];
            int count = 1;

            if (col + 1 < config.width) {
                const uint8_t* pixel2 = bgraRow + (col + 1) * 4;
                sumB += pixel2[0];
                sumG += pixel2[1];
                sumR += pixel2[2];
                count++;
            }

            if (row + 1 < config.height) {
                const uint8_t* bgraRow2 = bgraData + (row + 1) * stride;
                const uint8_t* pixel3 = bgraRow2 + col * 4;
                sumB += pixel3[0];
                sumG += pixel3[1];
                sumR += pixel3[2];
                count++;

                if (col + 1 < config.width) {
                    const uint8_t* pixel4 = bgraRow2 + (col + 1) * 4;
                    sumB += pixel4[0];
                    sumG += pixel4[1];
                    sumR += pixel4[2];
                    count++;
                }
            }

            sumB /= count;
            sumG /= count;
            sumR /= count;

            u[uvIndex] = std::min(255, std::max(0,
                uTable_r[sumR] + uTable_g[sumG] + uTable_b[sumB] + 128));
            v[uvIndex] = std::min(255, std::max(0,
                vTable_r[sumR] + vTable_g[sumG] + vTable_b[sumB] + 128));
        }
    }

    return true;
}

void ScreenCapture::handleCaptureError(HRESULT hr) {
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        Logger::getInstance()->warning("Access lost, reinitializing...");
        invalidCallCount++;

        if (!desktopSwitchInProgress && invalidCallCount == 2) {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToWinLogonDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = true;
        }
        else {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToDefaultDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = false;
        }

    }
    else if (hr == DXGI_ERROR_INVALID_CALL) {

        invalidCallCount++;

        if (!desktopSwitchInProgress && invalidCallCount == 2) {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToWinLogonDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = true;
        }
        else {
            this->releaseResourceDXGI();
            winLogonSwitcher->SwitchToDefaultDesktop();
            this->initializeDXGI();
            this->initializeGPUConverter();
            desktopSwitchInProgress = false;
        }

    }
}

void ScreenCapture::stopCapture() {
    if (!capturing.load()) {
        Logger::getInstance()->info("Capture not running, nothing to stop");
        return;
    }

    Logger::getInstance()->info("Stopping capture...");

    capturing = false;
    encoderRunning = false;

    encoderChannel.close();
    encoderContext.stop();

    if (captureThread.joinable()) {
        captureThread.join();
    }

    if (encoderThread.joinable()) {
        encoderThread.join();
    }

    std::shared_ptr<CapturedFrame> frame;
    int remainingFrames = 0;
    while (frameQueue.try_dequeue(frame)) {
        returnFrameToPool(frame);
        remainingFrames++;
    }

    if (remainingFrames > 0) {
        Logger::getInstance()->info("Cleared " + std::to_string(remainingFrames) + " remaining frames from queue");
    }

    Logger::getInstance()->info("Capture stopped successfully");
}

void ScreenCapture::releaseResources() {
    stopCapture();

    if (dxgiDuplication) {
        dxgiDuplication.Reset();
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        stagingTextures[i].Reset();
    }

    workingTexture.Reset();
    gpuEncoderTexture.Reset();
    gpuEncoderUAV.Reset();

    dxgiOutput5.Reset();
    dxgiOutput1.Reset();
    dxgiOutput.Reset();
    dxgiAdapter.Reset();
    dxgiDevice.Reset();
    d3dContext1.Reset();
    d3dContext.Reset();
    d3dDevice1.Reset();
    d3dDevice.Reset();

    yuvBuffer.clear();
    yuvBuffer.shrink_to_fit();

    if (yuvComputeShader) {
        yuvComputeShader.Reset();
    }
    if (yuvOutputBuffer) {
        yuvOutputBuffer.Reset();
    }
    if (yuvUAV) {
        yuvUAV.Reset();
    }
    if (stagingBuffer) {
        stagingBuffer.Reset();
    }

    std::shared_ptr<CapturedFrame> frame;
    while (framePool.try_dequeue(frame)) {
        // Clear pool
    }

    Logger::getInstance()->info("Resources released");
}

void ScreenCapture::releaseResourceDXGI() {
    if (dxgiDuplication) {
        dxgiDuplication.Reset();
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        stagingTextures[i].Reset();
    }

    workingTexture.Reset();


    dxgiOutput5.Reset();
    dxgiOutput1.Reset();
    dxgiOutput.Reset();
    dxgiAdapter.Reset();
    dxgiDevice.Reset();
    d3dContext1.Reset();
    d3dContext.Reset();
    d3dDevice1.Reset();
    d3dDevice.Reset();

    yuvBuffer.clear();
    yuvBuffer.shrink_to_fit();

    if (yuvComputeShader) {
        yuvComputeShader.Reset();
    }
    if (yuvOutputBuffer) {
        yuvOutputBuffer.Reset();
    }
    if (yuvUAV) {
        yuvUAV.Reset();
    }
    if (stagingBuffer) {
        stagingBuffer.Reset();
    }

}