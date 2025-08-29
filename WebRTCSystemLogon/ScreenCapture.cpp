#include "ScreenCapture.h"
#include <algorithm>
#include <thread>
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

                desktopSwitchInProgress = true;

            }
            else {

                this->releaseResourceDXGI();

                winLogonSwitcher->SwitchToDefaultDesktop();

                this->initializeDXGI();

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
    hr = dxgiDuplication->AcquireNextFrame(0, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 正常情况，没有新帧
        return true;
    }

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            this->releaseResourceDXGI();
            this->initializeDXGI();
            hasFrame = false;
            return false;
        }

        if (hr == DXGI_ERROR_INVALID_CALL) {

            if (!desktopSwitchInProgress) {

                this->releaseResourceDXGI();

                winLogonSwitcher->SwitchToWinLogonDesktop();

                this->initializeDXGI();

                desktopSwitchInProgress = true;

            }
            else {

                this->releaseResourceDXGI();

                winLogonSwitcher->SwitchToDefaultDesktop();

                this->initializeDXGI();

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

    // 处理更新区域（局部刷新）
    if (frameInfo.TotalMetadataBufferSize > 0) {
        processUpdateRegions(&frameInfo);
    }

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

void ScreenCapture::processUpdateRegions(DXGI_OUTDUPL_FRAME_INFO* frameInfo) {
    if (frameInfo->TotalMetadataBufferSize == 0) {
        return;
    }

    std::vector<BYTE> metadataBuffer(frameInfo->TotalMetadataBufferSize);
    UINT bufferSize = frameInfo->TotalMetadataBufferSize;

    // 获取移动矩形
    DXGI_OUTDUPL_MOVE_RECT* moveRects = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadataBuffer.data());
    UINT moveCount = 0;
    HRESULT hr = dxgiDuplication->GetFrameMoveRects(bufferSize, moveRects, &moveCount);

    if (SUCCEEDED(hr) && moveCount > 0) {
        Logger::getInstance()->debug("Move rects: " + std::to_string(moveCount));
    }

    // 获取脏矩形
    RECT* dirtyRectsPtr = reinterpret_cast<RECT*>(metadataBuffer.data());
    UINT dirtyCount = 0;
    hr = dxgiDuplication->GetFrameDirtyRects(bufferSize, dirtyRectsPtr, &dirtyCount);

    if (SUCCEEDED(hr) && dirtyCount > 0) {
        dirtyRects.clear();
        dirtyRects.reserve(dirtyCount);
        for (UINT i = 0; i < dirtyCount; i++) {
            dirtyRects.push_back(dirtyRectsPtr[i]);
        }
        Logger::getInstance()->debug("Dirty rects: " + std::to_string(dirtyCount));
    }
}

bool ScreenCapture::processFrame(ID3D11Texture2D* texture) {
    if (!texture || !d3dContext || !stagingTextures[currentTexture]) {
        Logger::getInstance()->error("Invalid texture or D3D context");
        return false;
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

    // 优化1: 只在有脏矩形时才保存上一帧
    bool hasChanges = !dirtyRects.empty();

    if (hasChanges && !lastFrameData.empty()) {
        // 优化2: 合并相邻的脏矩形以减少拷贝次数
        std::vector<RECT> mergedRects;
        mergedRects.reserve(dirtyRects.size());

        // 简单的矩形合并 - 如果矩形相距很近就合并
        for (const auto& rect : dirtyRects) {
            bool merged = false;

            for (auto& mergedRect : mergedRects) {
                // 如果矩形相距小于32像素，就合并
                if (abs(rect.left - mergedRect.right) < 32 ||
                    abs(mergedRect.left - rect.right) < 32) {
                    if (abs(rect.top - mergedRect.top) < 100 &&
                        abs(rect.bottom - mergedRect.bottom) < 100) {
                        // 合并矩形
                        mergedRect.left = std::min(mergedRect.left, rect.left);
                        mergedRect.top = std::min(mergedRect.top, rect.top);
                        mergedRect.right = std::max(mergedRect.right, rect.right);
                        mergedRect.bottom = std::max(mergedRect.bottom, rect.bottom);
                        merged = true;
                        break;
                    }
                }
            }

            if (!merged) {
                mergedRects.push_back(rect);
            }
        }

        // 优化3: 使用 memcpy 批量拷贝而不是逐行拷贝
        if (mergedRects.size() < dirtyRects.size() / 2 || mergedRects.size() > 10) {
            // 如果合并效果不好或者矩形太多，就全帧拷贝
            if (mapped.RowPitch == bytesPerRow) {
                memcpy(dstData, srcData, bytesPerRow * config.height);
            }
            else {
                for (int row = 0; row < config.height; ++row) {
                    memcpy(dstData + row * bytesPerRow,
                        srcData + row * mapped.RowPitch,
                        bytesPerRow);
                }
            }
        }
        else {
            // 先拷贝上一帧
            memcpy(dstData, lastFrameData.data(), lastFrameData.size());

            // 只更新合并后的脏矩形区域
            for (const auto& rect : mergedRects) {
                int left = std::max(0L, rect.left);
                int top = std::max(0L, rect.top);
                int right = std::min((long)config.width, rect.right);
                int bottom = std::min((long)config.height, rect.bottom);

                if (left < right && top < bottom) {
                    int width = right - left;

                    // 优化4: 批量拷贝整个矩形区域而不是逐行
                    if (mapped.RowPitch == bytesPerRow) {
                        // pitch相同，可以一次性拷贝整个矩形
                        const uint8_t* srcRect = srcData + top * mapped.RowPitch + left * 4;
                        uint8_t* dstRect = dstData + top * bytesPerRow + left * 4;
                        size_t rectSize = width * 4 * (bottom - top);
                        memcpy(dstRect, srcRect, rectSize);
                    }
                    else {
                        // pitch不同，按行拷贝
                        for (int row = top; row < bottom; ++row) {
                            const uint8_t* srcRow = srcData + row * mapped.RowPitch + left * 4;
                            uint8_t* dstRow = dstData + row * bytesPerRow + left * 4;
                            memcpy(dstRow, srcRow, width * 4);
                        }
                    }
                }
            }
        }
    }
    else {
        // 全帧拷贝
        if (mapped.RowPitch == bytesPerRow) {
            memcpy(dstData, srcData, bytesPerRow * config.height);
        }
        else {
            for (int row = 0; row < config.height; ++row) {
                memcpy(dstData + row * bytesPerRow,
                    srcData + row * mapped.RowPitch,
                    bytesPerRow);
            }
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

    return true;
}

void ScreenCapture::encodeFrame(std::shared_ptr<CapturedFrame> frame) {
    if (!frame) {
        Logger::getInstance()->error("encodeFrame: frame is null");
        return;
    }


    if (!convertBGRAToYUV420(frame->bgraData.data(), frame->stride, yuvBuffer)) {
        Logger::getInstance()->error("Failed to convert BGRA to YUV420");
        return;
    }

    if (frameCallback) {

        frameCallback(yuvBuffer.data(), yuvBuffer.size(), frame->width, frame->height);

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
}
