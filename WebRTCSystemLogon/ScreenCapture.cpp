#include "ScreenCapture.h"
#include <algorithm>
#include <thread>
#include <d3dcompiler.h> 
#include "Logger.h"

// RAII 辅助类
class ScopeGuard {
public:
    template<typename Func>
    explicit ScopeGuard(Func&& func) : m_func(std::forward<Func>(func)), m_active(true) {}

    ~ScopeGuard() {
        if (m_active) {
            m_func();
        }
    }

    void dismiss() { m_active = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    std::function<void()> m_func;
    bool m_active;
};

ScreenCapture::ScreenCapture()
    : encoderChannel(encoderContext, 128) {

    encoderWorkGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(encoderContext));

    winLogonSwitcher = std::make_unique<WinLogon>();

    winLogonSwitcher->SwitchToDefaultDesktop();

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


    // Check if callback is set
    if (!frameCallback) {
        Logger::getInstance()->warning("frameCallback not set! You must call setFrameCallback() before startCapture()!");
    }


    DWORD sessionId = 0;
    DWORD processId = GetCurrentProcessId();
    ProcessIdToSessionId(processId, &sessionId);
    Logger::getInstance()->info("Current session ID: " + std::to_string(sessionId));

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
        Logger::getInstance()->debug("Created new frame, pool was empty");
    }
    return frame;
}

void ScreenCapture::returnFrameToPool(std::shared_ptr<CapturedFrame> frame) {
    if (frame && framePool.size_approx() < 30) {
        framePool.enqueue(frame);
    }
}

bool ScreenCapture::initializeDXGI() {
    HRESULT hr;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    createFlags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    createFlags |= D3D11_CREATE_DEVICE_SINGLETHREADED;

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

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = config.width;
    desc.Height = config.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    // 创建三个 staging textures
    for (int i = 0; i < 3; i++) {
        hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextures[i]);
        if (FAILED(hr)) {
            Logger::getInstance()->error("Failed to create staging texture " + std::to_string(i));
            return false;
        }
    }

    return true;
}

// 修正 initializeGPUConverter 函数中的shader
bool ScreenCapture::initializeGPUConverter() {
    // 修正后的Compute Shader源码
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
            
            // 读取BGRA像素
            float4 color = inputTexture.Load(int3(id.x, id.y, 0));
            
            // 关键修正：BGRA格式的正确映射
            // DirectX中 BGRA 格式实际存储顺序是 B-G-R-A
            // 但在shader中读取时：
            // color.b = B通道 (蓝色) 
            // color.g = G通道 (绿色)
            // color.r = R通道 (红色)
            // color.a = A通道 (透明度)
            
            float b = color.b * 255.0f;  // 修正：B在b通道
            float g = color.g * 255.0f;  // G在g通道
            float r = color.r * 255.0f;  // 修正：R在r通道
            
            // 使用CPU相同的公式
            uint y = (uint)((66 * r + 129 * g + 25 * b) / 256 + 16);
            y = min(y, 255u);
            
            // 写入Y数据
            uint yIndex = id.y * width + id.x;
            uint arrayIndex = yIndex / 4;
            uint byteOffset = yIndex % 4;
            uint shiftAmount = byteOffset * 8;
            uint mask = 0xFF << shiftAmount;
            uint value = y << shiftAmount;
            
            InterlockedAnd(outputBuffer[arrayIndex], ~mask);
            InterlockedOr(outputBuffer[arrayIndex], value);
            
            // UV通道处理
            if ((id.x % 2 == 0) && (id.y % 2 == 0)) {
                float4 c00 = color;
                float4 c10 = inputTexture.Load(int3(min(id.x + 1, width - 1), id.y, 0));
                float4 c01 = inputTexture.Load(int3(id.x, min(id.y + 1, height - 1), 0));
                float4 c11 = inputTexture.Load(int3(min(id.x + 1, width - 1), min(id.y + 1, height - 1), 0));
                
                // 修正通道映射
                float4 avg = (c00 + c10 + c01 + c11) * 0.25f * 255.0f;
                float avgB = avg.b;  // 修正：B在b通道
                float avgG = avg.g;  // G在g通道
                float avgR = avg.r;  // 修正：R在r通道
                
                // 使用CPU相同的公式
                uint u = (uint)((-38 * avgR - 74 * avgG + 112 * avgB) / 256 + 128);
                uint v = (uint)((112 * avgR - 94 * avgG - 18 * avgB) / 256 + 128);
                u = clamp(u, 0u, 255u);
                v = clamp(v, 0u, 255u);
                
                uint uvIndex = (id.y / 2) * (width / 2) + (id.x / 2);
                
                // U数据
                uint uPos = ySize + uvIndex;
                uint uArrayIndex = uPos / 4;
                uint uByteOffset = uPos % 4;
                uint uShift = uByteOffset * 8;
                
                InterlockedAnd(outputBuffer[uArrayIndex], ~(0xFF << uShift));
                InterlockedOr(outputBuffer[uArrayIndex], u << uShift);
                
                // V数据
                uint vPos = ySize + uvSize + uvIndex;
                uint vArrayIndex = vPos / 4;
                uint vByteOffset = vPos % 4;
                uint vShift = vByteOffset * 8;
                
                InterlockedAnd(outputBuffer[vArrayIndex], ~(0xFF << vShift));
                InterlockedOr(outputBuffer[vArrayIndex], v << vShift);
            }
        }
    )";

    // 编译Compute Shader
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

    // 创建输出缓冲区（使用StructuredBuffer）
    const UINT yuvBufferSize = config.width * config.height * 3 / 2;
    const UINT bufferSizeInUints = (yuvBufferSize + 3) / 4;  // 向上取整到4字节边界

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = bufferSizeInUints * sizeof(UINT);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(UINT);

    // 初始化为0
    std::vector<UINT> initialData(bufferSizeInUints, 0);
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = initialData.data();

    hr = d3dDevice->CreateBuffer(&bufferDesc, &initData, &yuvOutputBuffer);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create output buffer");
        return false;
    }

    // 创建UAV
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

    // 创建staging buffer用于读回CPU
    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.BindFlags = 0;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufferDesc.MiscFlags = 0;

    hr = d3dDevice->CreateBuffer(&bufferDesc, nullptr, &stagingBuffer);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create staging buffer");
        return false;
    }

    Logger::getInstance()->info("GPU YUV converter initialized successfully");
    return true;
}

// 修正 convertBGRAToYUV420_GPU 函数
bool ScreenCapture::convertBGRAToYUV420_GPU(ID3D11Texture2D* sourceTexture, std::vector<uint8_t>& yuvBuffer) {
    if (!yuvComputeShader || !sourceTexture) {
        return false;
    }

    // 创建源纹理的SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSRV;
    HRESULT hr = d3dDevice->CreateShaderResourceView(sourceTexture, &srvDesc, &srcSRV);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to create SRV");
        return false;
    }

    // 设置常量缓冲区
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
        Logger::getInstance()->error("Failed to create constant buffer");
        return false;
    }

    // 清空输出缓冲区（重要！避免残留数据）
    const UINT bufferSizeInUints = (constants.ySize * 3 / 2 + 3) / 4;
    std::vector<UINT> clearData(bufferSizeInUints, 0);
    d3dContext->UpdateSubresource(yuvOutputBuffer.Get(), 0, nullptr, clearData.data(), 0, 0);

    // 设置Compute Shader资源
    d3dContext->CSSetShader(yuvComputeShader.Get(), nullptr, 0);
    d3dContext->CSSetShaderResources(0, 1, srcSRV.GetAddressOf());
    d3dContext->CSSetUnorderedAccessViews(0, 1, yuvUAV.GetAddressOf(), nullptr);
    d3dContext->CSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());

    // Dispatch
    UINT dispatchX = (config.width + 15) / 16;
    UINT dispatchY = (config.height + 15) / 16;
    d3dContext->Dispatch(dispatchX, dispatchY, 1);

    // 清理绑定
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer* nullCB = nullptr;
    d3dContext->CSSetShaderResources(0, 1, &nullSRV);
    d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    d3dContext->CSSetConstantBuffers(0, 1, &nullCB);

    // 拷贝到staging buffer
    d3dContext->CopyResource(stagingBuffer.Get(), yuvOutputBuffer.Get());

    // 读回CPU
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = d3dContext->Map(stagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to map staging buffer");
        return false;
    }

    const int yuvSize = config.width * config.height * 3 / 2;
    if (yuvBuffer.size() != yuvSize) {
        yuvBuffer.resize(yuvSize);
    }

    // 只拷贝实际的YUV数据大小
    memcpy(yuvBuffer.data(), mapped.pData, yuvSize);
    d3dContext->Unmap(stagingBuffer.Get(), 0);

    return true;
}

bool ScreenCapture::startCapture() {
    if (capturing.load()) {
        Logger::getInstance()->warning("Capture already started");
        return true;
    }

    capturing = true;
    encoderRunning = true;

    // 重置 io_context（如果之前被停止过）
    encoderContext.restart();

    // 启动编码器线程 - 必须运行 io_context！
    encoderThread = std::thread([this]() {
        Logger::getInstance()->info("Encoder thread started - will run io_context");

        try {
            // 启动异步接收协程
            boost::asio::co_spawn(encoderContext, [this]() -> boost::asio::awaitable<void> {
                Logger::getInstance()->info("=== ENCODER COROUTINE STARTED AND RUNNING ===");

                while (encoderRunning.load()) {
                    try {
                        // 等待通知或超时（100ms超时避免死锁）
                        auto [ec] = co_await encoderChannel.async_receive(
                            boost::asio::as_tuple(boost::asio::use_awaitable));

                        if (ec) {
                            if (ec == boost::asio::error::operation_aborted) {
                                Logger::getInstance()->info("Encoder channel closed, stopping");
                                break;
                            }
                            // 超时或其他错误，继续检查队列
                            // 这很重要，因为可能有帧在队列中但没有通知
                        }

                        // 处理所有待编码帧
                        std::shared_ptr<CapturedFrame> frame;
                        int processedCount = 0;
                        int maxBatch = 5; // 批量处理限制，避免延迟过高

                        while (frameQueue.try_dequeue(frame) && processedCount < maxBatch) {
                            encodeFrame(frame);
                            returnFrameToPool(frame);
                        }

                    }
                    catch (const std::exception& e) {
                        Logger::getInstance()->error("Error in encoder coroutine: " + std::string(e.what()));

                    }
                }

                co_return;
                }, boost::asio::detached);

            // 这是关键！必须运行 io_context，否则协程永远不会执行！
            Logger::getInstance()->info("Starting io_context.run() - this blocks until stopped");
            size_t handlers = encoderContext.run();
            Logger::getInstance()->info("io_context.run() returned after processing " +
                std::to_string(handlers) + " handlers");
        }
        catch (const std::exception& e) {
            Logger::getInstance()->error("Fatal error in encoder thread: " + std::string(e.what()));
        }

        Logger::getInstance()->info("Encoder thread ending");
        });

    // 等待一下确保编码器线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动捕获线程
    captureThread = std::thread([this]() {
        Logger::getInstance()->info("Capture thread starting");
        captureThreadFunc();
        Logger::getInstance()->info("Capture thread ending");
        });

    Logger::getInstance()->info("Capture started successfully - both threads running");
    return true;
}

void ScreenCapture::captureThreadFunc() {
    Logger::getInstance()->info("Capture thread started");

    while (capturing.load()) {

        captureFrame();

    }

}


bool ScreenCapture::captureFrame() {
    // 检查是否需要重新初始化（桌面切换后）
    if (!dxgiDuplication) {
        this->releaseResourceDXGI();
        this->initializeDXGI();
        this->initializeGPUConverter();
        return false;
    }

    HRESULT hr;
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // 关键优化：在获取新帧前立即释放旧帧
    static bool hasFrame = false;
    if (hasFrame) {
        dxgiDuplication->ReleaseFrame();
        hasFrame = false;
    }

    // 使用 16ms 超时（约60fps）
    hr = dxgiDuplication->AcquireNextFrame(100, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 正常情况，没有新帧
        return true;
    }

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            this->releaseResourceDXGI();
            this->initializeDXGI();
            this->initializeGPUConverter();
            hasFrame = false;
            return false;
        }

        if (hr == DXGI_ERROR_INVALID_CALL) {
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

            Logger::getInstance()->info("desktopSwitchInProgress");
            hasFrame = false;
            return false;
        }
        Logger::getInstance()->error("AcquireNextFrame failed: 0x" +
            std::to_string(static_cast<unsigned int>(hr)));
        hasFrame = false;
        return false;
    }

    hasFrame = true;

    // 注释掉脏矩形处理
    // if (frameInfo.TotalMetadataBufferSize > 0) {
    //     processUpdateRegions(&frameInfo);
    // }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTexture;
    hr = desktopResource.As(&acquiredTexture);
    if (FAILED(hr)) {
        Logger::getInstance()->error("Failed to get texture from desktop resource");
        return false;
    }

    bool result = processFrame(acquiredTexture.Get());
    if (!result) {
        Logger::getInstance()->error("processFrame failed");
    }

    return result;
}

// 2. processFrame() 函数 - 简化，去掉脏矩形逻辑
bool ScreenCapture::processFrame(ID3D11Texture2D* texture) {
    if (!texture || !d3dContext || !stagingTextures[currentTexture]) {
        Logger::getInstance()->error("Invalid texture or D3D context");
        return false;
    }

    bool gpuSuccess = false;
    if (yuvComputeShader) {
        // 直接使用源纹理进行GPU转换
        std::vector<uint8_t> tempYuvBuffer;
        gpuSuccess = convertBGRAToYUV420_GPU(texture, tempYuvBuffer);

        if (gpuSuccess) {
            // GPU转换成功，直接创建帧
            auto frame = getFrameFromPool();
            if (!frame) {
                return false;
            }

            // 将YUV数据复制到frame（这里可以优化为直接使用YUV数据）
            frame->bgraData = std::move(tempYuvBuffer);
            frame->width = config.width;
            frame->height = config.height;
            frame->stride = config.width;  // YUV的stride不同
            frame->timestamp = std::chrono::steady_clock::now();

            frameQueue.enqueue(frame);

            if (encoderChannel.is_open()) {
                boost::system::error_code ec;
                encoderChannel.try_send(ec);
            }

            return true;
        }
    }

    d3dContext->CopyResource(stagingTextures[currentTexture].Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped;

    // 尝试非阻塞映射
    HRESULT hr = d3dContext->Map(
        stagingTextures[currentTexture].Get(),
        0,
        D3D11_MAP_READ,
        0,  // 非阻塞标志
        &mapped);

    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        Logger::getInstance()->info("Staging texture still in use by GPU, skipping frame");
        // GPU 还在使用这个缓冲区，切换到下一个
        currentTexture = (currentTexture + 1) % 3;
        return false;  // 跳过本帧
    }

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

    // 简化：总是全帧拷贝，去掉脏矩形优化
    if (mapped.RowPitch == bytesPerRow) {
        // pitch相同，可以一次性拷贝
        memcpy(dstData, srcData, bytesPerRow * config.height);
    }
    else {
        // pitch不同，按行拷贝
        for (int row = 0; row < config.height; ++row) {
            memcpy(dstData + row * bytesPerRow,
                srcData + row * mapped.RowPitch,
                bytesPerRow);
        }
    }

    d3dContext->Unmap(stagingTextures[currentTexture].Get(), 0);

    frame->width = config.width;
    frame->height = config.height;
    frame->stride = bytesPerRow;
    frame->timestamp = std::chrono::steady_clock::now();

    // 队列管理
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

    // 切换到下一个缓冲区
    currentTexture = (currentTexture + 1) % 3;

    return true;
}

void ScreenCapture::encodeFrame(std::shared_ptr<CapturedFrame> frame) {
    if (!frame) {
        Logger::getInstance()->error("encodeFrame: frame is null");
        return;
    }

    // 检查是否已经是YUV格式（GPU转换的结果）
    if (frame->stride == config.width) {  // YUV格式的标志
        // 直接使用，不需要转换
        if (frameCallback) {
            frameCallback(frame->bgraData.data(), frame->bgraData.size(),
                frame->width, frame->height);
        }
    }
    else {
        // BGRA格式，需要CPU转换
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


void ScreenCapture::stopCapture() {
    if (!capturing.load()) {
        Logger::getInstance()->info("Capture not running, nothing to stop");
        return;
    }

    Logger::getInstance()->info("Stopping capture...");

    // 设置停止标志
    capturing = false;
    encoderRunning = false;

    // 关闭channel并停止context
    Logger::getInstance()->info("Closing encoder channel");
    encoderChannel.close();

    Logger::getInstance()->info("Stopping encoder context");
    encoderContext.stop();

    // 等待捕获线程结束
    if (captureThread.joinable()) {
        Logger::getInstance()->info("Waiting for capture thread to finish");
        captureThread.join();
        Logger::getInstance()->info("Capture thread finished");
    }

    // 等待编码器线程结束
    if (encoderThread.joinable()) {
        Logger::getInstance()->info("Waiting for encoder thread to finish");
        encoderThread.join();
        Logger::getInstance()->info("Encoder thread finished");
    }

    // 清空队列
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

    for (int i = 0; i < 3; i++) {

        stagingTextures[i].Reset();

    }

    dxgiOutput1.Reset();
    dxgiOutput.Reset();
    dxgiAdapter.Reset();
    dxgiDevice.Reset();
    d3dContext.Reset();
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


void ScreenCapture::releaseResourceDXGI()
{
    if (dxgiDuplication) {
        dxgiDuplication.Reset();
    }

    for (int i = 0; i < 3; i++) {

        stagingTextures[i].Reset();

    }

    dxgiOutput1.Reset();
    dxgiOutput.Reset();
    dxgiAdapter.Reset();
    dxgiDevice.Reset();
    d3dContext.Reset();
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
