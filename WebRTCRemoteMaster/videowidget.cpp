#include "videowidget.h"
#include "webrtcmanager.h"
#include <QVBoxLayout>
#include <QFile>
#include <QCursor>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QDir>
#include <algorithm>
#include "Utils.h"

namespace hope{

namespace rtc{
VideoWidget::VideoWidget(QWidget* parent)
    : QRhiWidget(parent)
    , manager(nullptr)
    , rhi(nullptr)
    , videoWidth(640)
    , videoHeight(480)
    , resourcesInitialized(false)
    , fullScreenButton(nullptr)
    , sidebar(nullptr)
    , sidebarExitButton(nullptr)
    , mouseCheckTimer(nullptr)
    , hideTimer(nullptr)
    , sidebarAnimation(nullptr)
    , isFullScreenMode(false)
    , sidebarVisible(false)
    ,interceptionHook(nullptr)
{
    LOG_INFO("VideoWidget init");

    QIcon windowIcon(":/logo/res/hope.png");
    if (!windowIcon.isNull()) {
        setWindowIcon(windowIcon);
    }

    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    // Initialize FPS timer
    fpsTimer.start();
    QTimer* fpsUpdateTimer = new QTimer(this);
    connect(fpsUpdateTimer, &QTimer::timeout, this, &VideoWidget::updateFPS);
    fpsUpdateTimer->start(1000);

    // Initialize timestamp
    lastUpdateTime = std::chrono::steady_clock::now();

    // Initialize uniform cache
    for (auto& uniformData : lastUniformData) {
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
    }

    initializeControls();
    loadPipelineCache();

    LOG_INFO("VideoWidget init finished");
}

VideoWidget::~VideoWidget()
{
    LOG_INFO("VideoWidget destruction");
    savePipelineCache();
}

void VideoWidget::initialize(QRhiCommandBuffer* cb)
{
    if (!QRhiWidget::rhi()) {
        LOG_ERROR("RHI not initialized");
        return;
    }

    if (rhi != QRhiWidget::rhi()) {
        LOG_INFO("RHI instance changed, recreating resources");
        releaseResources();
        rhi = QRhiWidget::rhi();
        resourcesInitialized = false;
    }

    if (!resourcesInitialized) {
        LOG_INFO("Starting video rendering resource initialization");

        if (!initializeResources(cb)) {
            LOG_ERROR("Resource initialization failed");
            return;
        }

        resourcesInitialized = true;
        LOG_INFO("Video rendering resource initialization completed");
    }
}

bool VideoWidget::initializeResources(QRhiCommandBuffer* cb)
{
    createBuffers();
    createTextures();
    createSampler();
    createShaderResourceBindings();
    createPipeline();

    if (cb && rhi) {
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

        // Upload vertex data
        static const float vertexData[] = {
            -1.0f,  1.0f,  0.0f, 0.0f,
            -1.0f, -1.0f,  0.0f, 1.0f,
            1.0f, -1.0f,  1.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
            1.0f, -1.0f,  1.0f, 1.0f,
            1.0f,  1.0f,  1.0f, 0.0f
        };
        batch->uploadStaticBuffer(vertexBuffer.get(), vertexData);

        // Initialize uniform buffers
        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(
            hasVideo ? 1.0f : 0.0f,
            0.0f,
            1.0f,
            0.0f
            );

        for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            batch->updateDynamicBuffer(uniformBuffers[i].get(), 0, sizeof(UniformData), &uniformData);
            lastUniformData[i] = uniformData;
        }

        cb->resourceUpdate(batch);
    }

    bool success = (pipeline != nullptr);
    if (!success) {
        LOG_ERROR("initializeResources failed");
    }
    return success;
}

void VideoWidget::createBuffers()
{
    if (!rhi) {
        LOG_ERROR("createBuffers: RHI is null");
        return;
    }

    vertexBuffer.reset(rhi->newBuffer(
        QRhiBuffer::Immutable,
        QRhiBuffer::VertexBuffer,
        6 * 4 * sizeof(float)
        ));

    if (!vertexBuffer || !vertexBuffer->create()) {
        LOG_ERROR("Vertex buffer creation failed");
        return;
    }

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        uniformBuffers[i].reset(rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            sizeof(UniformData)
            ));

        if (!uniformBuffers[i] || !uniformBuffers[i]->create()) {
            LOG_ERROR("Uniform buffer %d creation failed", i);
            return;
        }
    }
}

void VideoWidget::createTextures()
{
    if (!rhi) {
        LOG_ERROR("createTextures: RHI is null");
        return;
    }

    // Use fixed maximum size to avoid frequent texture reconstruction
    QSize initialSize(MAX_TEXTURE_WIDTH, MAX_TEXTURE_HEIGHT);

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        // Key modification: Added UsedAsTransferSource flag, create Dynamic texture
        videoTextures[i].reset(rhi->newTexture(
            QRhiTexture::RGBA8,
            initialSize,
            1,
            QRhiTexture::UsedAsTransferSource  // Enable dynamic upload optimization
            ));

        if (!videoTextures[i]) {
            LOG_ERROR("Failed to create video texture object %d", i);
            return;
        }

        if (!videoTextures[i]->create()) {
            LOG_WARNING("Failed to create video texture %d, trying to create fallback texture", i);

            // Fallback texture also uses Dynamic flag
            videoTextures[i].reset(rhi->newTexture(
                QRhiTexture::RGBA8,
                QSize(1280, 720),
                1,
                QRhiTexture::UsedAsTransferSource
                ));

            if (!videoTextures[i] || !videoTextures[i]->create()) {
                LOG_ERROR("Failed to create fallback texture %d", i);
                return;
            }
        }
    }

    LOG_INFO("Dynamic texture creation completed, size: %dx%d", initialSize.width(), initialSize.height());
}

void VideoWidget::createSampler()
{
    if (!rhi) {
        LOG_ERROR("createSampler: RHI is null");
        return;
    }

    sampler.reset(rhi->newSampler(
        QRhiSampler::Linear,
        QRhiSampler::Linear,
        QRhiSampler::None,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge
        ));

    if (!sampler || !sampler->create()) {
        LOG_ERROR("Sampler creation failed");
        return;
    }
}

void VideoWidget::createShaderResourceBindings()
{
    if (!rhi) {
        LOG_ERROR("createShaderResourceBindings: RHI is null");
        return;
    }

    if (!uniformBuffers[0] || !videoTextures[0] || !sampler) {
        LOG_ERROR("Dependent resources not ready");
        return;
    }

    srb.reset(rhi->newShaderResourceBindings());
    if (!srb) {
        LOG_ERROR("Failed to create SRB object");
        return;
    }

    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffers[0].get()
            ),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::FragmentStage,
            videoTextures[0].get(),
            sampler.get()
            )
    });

    if (!srb->create()) {
        LOG_ERROR("Main shader resource binding create() failed");
        srb.reset();
        return;
    }

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        perFrameSrb[i].reset(rhi->newShaderResourceBindings());
        if (!perFrameSrb[i]) {
            LOG_ERROR("Failed to create frame %d SRB object", i);
            continue;
        }

        perFrameSrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                uniformBuffers[i].get()
                ),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                videoTextures[i].get(),
                sampler.get()
                )
        });

        if (!perFrameSrb[i]->create()) {
            LOG_ERROR("Frame %d shader resource binding create() failed", i);
            perFrameSrb[i].reset();
        }
    }
}

void VideoWidget::createPipeline()
{
    if (!rhi) {
        LOG_ERROR("createPipeline: RHI is null");
        return;
    }

    if (!srb) {
        LOG_ERROR("Cannot create pipeline: shader resource binding not ready");
        return;
    }

    pipeline.reset(rhi->newGraphicsPipeline());
    if (!pipeline) {
        LOG_ERROR("Failed to create pipeline object");
        return;
    }

    QShader vertShader = getShader(":/shaders/res/video.vert.qsb");
    QShader fragShader = getShader(":/shaders/res/video.frag.qsb");

    if (!vertShader.isValid() || !fragShader.isValid()) {
        LOG_ERROR("Shader invalid - vertex: %s, fragment: %s",
                 vertShader.isValid() ? "valid" : "invalid",
                 fragShader.isValid() ? "valid" : "invalid");
        pipeline.reset();
        return;
    }

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vertShader },
        { QRhiShaderStage::Fragment, fragShader }
    });

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        { 4 * sizeof(float) }
    });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) }
    });

    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    pipeline->setDepthTest(false);
    pipeline->setDepthWrite(false);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);

    if (!pipeline->create()) {
        LOG_ERROR("Render pipeline create() failed");
        pipeline.reset();
        return;
    }
}

bool VideoWidget::needsTextureResize(int width, int height, int slot)
{
    if (!videoTextures[slot]) return true;

    QSize currentSize = videoTextures[slot]->pixelSize();

    // Optimization: Only rebuild when size change exceeds threshold
    int widthDiff = std::abs(currentSize.width() - width);
    int heightDiff = std::abs(currentSize.height() - height);

    // If new size exceeds current texture size, need to rebuild
    if (width > currentSize.width() || height > currentSize.height()) {
        return true;
    }

    // If new size is much smaller than current texture (save video memory), also consider rebuilding
    if (widthDiff > MIN_TEXTURE_RESIZE_THRESHOLD * 2 &&
        heightDiff > MIN_TEXTURE_RESIZE_THRESHOLD * 2) {
        return true;
    }

    return false;
}

void VideoWidget::resizeTextureIfNeeded(int slot, const QSize& newSize)
{
    // Rebuild texture with Dynamic flag
    videoTextures[slot].reset(rhi->newTexture(
        QRhiTexture::RGBA8,
        newSize,
        1,
        QRhiTexture::UsedAsTransferSource  // Dynamic texture
        ));

    if (videoTextures[slot] && videoTextures[slot]->create()) {
        // Rebuild shader resource binding
        perFrameSrb[slot].reset(rhi->newShaderResourceBindings());
        perFrameSrb[slot]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                uniformBuffers[slot].get()
                ),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                videoTextures[slot].get(),
                sampler.get()
                )
        });
        perFrameSrb[slot]->create();

        LOG_INFO("Dynamic texture rebuilt, slot: %d, size: %dx%d", slot, newSize.width(), newSize.height());
    } else {
        LOG_ERROR("Failed to rebuild dynamic texture");
    }
}

void VideoWidget::render(QRhiCommandBuffer* cb)
{
    if (!rhi || !resourcesInitialized || !pipeline) {
        const QColor clearColor(32, 32, 32);
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    QRhiResourceUpdateBatch* batch = nullptr;
    int frameToRender = -1;

    // Find the latest available frame
    {
        for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            int slotIndex = (currentFrameSlot.load(std::memory_order_acquire) - i + FRAME_BUFFER_COUNT) % FRAME_BUFFER_COUNT;
            if (frameBuffers[slotIndex].ready && frameBuffers[slotIndex].data) {
                frameToRender = slotIndex;
                break;
            }
        }
    }

    if (frameToRender >= 0) {
        batch = rhi->nextResourceUpdateBatch();
        FrameBuffer& frame = frameBuffers[frameToRender];

        if (frame.needsUpdate && frame.ready && frame.data) {
            // Check if texture needs to be rebuilt
            if (needsTextureResize(frame.width, frame.height, frameToRender)) {
                QSize newSize(
                    std::min(frame.width, MAX_TEXTURE_WIDTH),
                    std::min(frame.height, MAX_TEXTURE_HEIGHT)
                    );
                resizeTextureIfNeeded(frameToRender, newSize);
            }

            if (videoTextures[frameToRender]) {
                size_t rgbaSize = frame.width * frame.height * 4;

                // Optimization: For Dynamic textures, this upload is more efficient
                // QRhi internally uses staging buffer or mapped memory
                QRhiTextureSubresourceUploadDescription subresDesc(
                    frame.data.get(),
                    rgbaSize
                    );
                subresDesc.setSourceSize(QSize(frame.width, frame.height));
                subresDesc.setDestinationTopLeft(QPoint(0, 0));

                QRhiTextureUploadDescription desc({ 0, 0, subresDesc });
                batch->uploadTexture(videoTextures[frameToRender].get(), desc);

                frame.needsUpdate = false;
                hasVideo = true;
                videoWidth = frame.width;
                videoHeight = frame.height;
            }
        }

        // Update uniform buffer (only when data changes)
        if (frameToRender >= 0 && videoTextures[frameToRender]) {
            UniformData uniformData;
            uniformData.mvp.setToIdentity();
            uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f);

            if (uniformData != lastUniformData[frameToRender]) {
                batch->updateDynamicBuffer(uniformBuffers[frameToRender].get(),
                                           0, sizeof(UniformData), &uniformData);
                lastUniformData[frameToRender] = uniformData;
            }
        }
    }

    const QColor clearColor = hasVideo ? Qt::black : QColor(48, 48, 48);
    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, batch);

    if (frameToRender >= 0 && perFrameSrb[frameToRender]) {
        const QSize outputSize = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(pipeline.get());
        cb->setViewport({ 0, 0, float(outputSize.width()), float(outputSize.height()) });
        cb->setShaderResources(perFrameSrb[frameToRender].get());

        const QRhiCommandBuffer::VertexInput vbufBinding(vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(6);

        frameCount++;
    }

    cb->endPass();
}

void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame || !frame->data) {
        return;
    }

    // Optimization: Frame rate limiting to avoid excessive refresh
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdateTime);

    if (elapsed.count() < MIN_FRAME_INTERVAL_MS) {
        return; // Skip this frame
    }

    int nextSlot = (currentFrameSlot.load(std::memory_order_relaxed) + 1) % FRAME_BUFFER_COUNT;

    // Optimization: Prepare temporary data to reduce lock holding time
    FrameBuffer tempBuffer;
    tempBuffer.data = frame->data;
    tempBuffer.width = frame->width;
    tempBuffer.height = frame->height;
    tempBuffer.ready = true;
    tempBuffer.needsUpdate = true;

    {
        std::lock_guard<std::mutex> lock(frameMutex);
        frameBuffers[nextSlot] = std::move(tempBuffer);
        currentFrameSlot.store(nextSlot, std::memory_order_release);
    }

    update();
    lastUpdateTime = now;
}

void VideoWidget::clearDisplay()
{
    LOG_INFO("Clearing display");
    hasVideo = false;

    {
        std::lock_guard<std::mutex> lock(frameMutex);
        for (auto& buffer : frameBuffers) {
            buffer.ready = false;
            buffer.needsUpdate = false;
            buffer.data.reset();
        }
    }

    update();
}

void VideoWidget::releaseResources()
{
    LOG_INFO("Releasing rendering resources");

    pipeline.reset();
    srb.reset();

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        perFrameSrb[i].reset();
        uniformBuffers[i].reset();
        videoTextures[i].reset();
    }

    sampler.reset();
    vertexBuffer.reset();

    resourcesInitialized = false;
}

QShader VideoWidget::getShader(const QString& name)
{
    QFile f(name);
    if (f.open(QIODevice::ReadOnly)) {
        QShader shader = QShader::fromSerialized(f.readAll());
        if (shader.isValid()) {
            return shader;
        } else {
            LOG_ERROR("Shader invalid: %s", name.toStdString().c_str());
        }
    } else {
        LOG_ERROR("Cannot open shader file: %s", name.toStdString().c_str());
    }

    return QShader();
}

void VideoWidget::loadPipelineCache()
{
    // Optimization: Load pipeline cache to speed up startup
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QString cachePath = cacheDir + "/pipeline.cache";

    QFile cacheFile(cachePath);
    if (cacheFile.open(QIODevice::ReadOnly)) {
        QByteArray cacheData = cacheFile.readAll();
        if (!cacheData.isEmpty()) {
            LOG_INFO("Loading pipeline cache, size: %zu bytes", cacheData.size());
            // Note: Actually need to call after RHI initialization
            // rhi->setPipelineCacheData(cacheData);
        }
        cacheFile.close();
    }
}

void VideoWidget::savePipelineCache()
{
    if (!rhi) return;

    // Optimization: Save pipeline cache to speed up next startup
    QByteArray cacheData = rhi->pipelineCacheData();
    if (cacheData.isEmpty()) return;

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QString cachePath = cacheDir + "/pipeline.cache";

    QFile cacheFile(cachePath);
    if (cacheFile.open(QIODevice::WriteOnly)) {
        cacheFile.write(cacheData);
        cacheFile.close();
        LOG_INFO("Saving pipeline cache, size: %zu bytes", cacheData.size());
    }
}

void VideoWidget::updateFPS()
{
    qint64 elapsed = fpsTimer.elapsed();
    if (elapsed > 0) {
        double fps = (frameCount * 1000.0) / elapsed;
        currentFPS = fps;
        frameCount = 0;
        fpsTimer.restart();
    }
}

void VideoWidget::setWebRTCManager(WebRTCManager * manager)
{
    LOG_INFO("Setting WebRTC remote client");
    this->manager = manager;

    interceptionHook = std::make_unique<InterceptionHook>();
    interceptionHook->setTargetWidget(this);
    interceptionHook->setManager(manager);
    interceptionHook->setVideoSize(width(), height());
    interceptionHook->startCapture();
}

void VideoWidget::initializeControls()
{
    sidebar = new QWidget(this);
    sidebar->setFixedWidth(SIDEBAR_WIDTH);
    sidebar->setStyleSheet(R"(
        QWidget {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                        stop:0 rgba(0, 0, 0, 220),
                        stop:0.8 rgba(0, 0, 0, 200),
                        stop:1 rgba(0, 0, 0, 150));
            border-top-right-radius: 10px;
            border-bottom-right-radius: 10px;
        }
    )");
    sidebar->setVisible(false);

    QVBoxLayout* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(5, 10, 5, 10);
    sidebarLayout->setSpacing(5);

    fullScreenButton = new QPushButton("全\n屏", sidebar);
    fullScreenButton->setFixedSize(20, 20);
    fullScreenButton->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #667EEA, stop:1 #764BA2);
            color: white;
            border: none;
            border-radius: 2px;
            font-size: 8px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #5A67D8, stop:1 #6B46C1);
        }
    )");
    connect(fullScreenButton, &QPushButton::clicked, this, &VideoWidget::onFullScreenClicked);
    fullScreenButton->setVisible(true);

    sidebarExitButton = new QPushButton("退出\n全屏", sidebar);
    sidebarExitButton->setFixedSize(20, 20);
    sidebarExitButton->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #F56565, stop:1 #E53E3E);
            color: white;
            border: none;
            border-radius: 2px;
            font-size: 8px;
            font-weight: 600;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #E53E3E, stop:1 #C53030);
        }
    )");
    sidebarExitButton->setVisible(false);
    connect(sidebarExitButton, &QPushButton::clicked, this, &VideoWidget::onExitFullScreenClicked);

    sidebarLayout->addWidget(fullScreenButton);
    sidebarLayout->addWidget(sidebarExitButton);
    sidebarLayout->addStretch();

    sidebarAnimation = new QPropertyAnimation(sidebar, "pos", this);
    sidebarAnimation->setDuration(250);
    sidebarAnimation->setEasingCurve(QEasingCurve::OutCubic);

    mouseCheckTimer = new QTimer(this);
    mouseCheckTimer->setInterval(50);
    connect(mouseCheckTimer, &QTimer::timeout, this, &VideoWidget::checkMousePosition);

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    hideTimer->setInterval(HIDE_DELAY);
    connect(hideTimer, &QTimer::timeout, this, &VideoWidget::hideSidebar);

    mouseCheckTimer->start();

    sidebar->setVisible(true);
    sidebarVisible = true;
    updateControlsPosition();
}

void VideoWidget::updateControlsPosition()
{
    if (!sidebar) return;

    int sidebarHeight = height() * 0.4;
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebar->setFixedHeight(sidebarHeight);

    if (sidebarVisible) {
        sidebar->move(0, sidebarY);
    } else {
        sidebar->move(-SIDEBAR_WIDTH + 3, sidebarY);
    }

    if (isFullScreenMode) {
        fullScreenButton->setVisible(false);
        sidebarExitButton->setVisible(true);
    } else {
        fullScreenButton->setVisible(true);
        sidebarExitButton->setVisible(false);
    }
}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QRhiWidget::resizeEvent(event);
    if (interceptionHook) {
        interceptionHook->setVideoSize(event->size().width(), event->size().height());
    }
    updateControlsPosition();
}

void VideoWidget::enterFullScreen()
{
    if (isFullScreenMode) return;

    normalGeometry = geometry();
    normalWindowState = windowState();

    isFullScreenMode = true;
    setWindowState(Qt::WindowFullScreen);

    updateControlsPosition();
    LOG_INFO("Entering full screen mode");
}

void VideoWidget::exitFullScreen()
{
    if (!isFullScreenMode) return;

    sidebar->setVisible(false);
    sidebarVisible = false;

    isFullScreenMode = false;
    setWindowState(normalWindowState);
    setGeometry(normalGeometry);

    sidebar->setVisible(true);
    sidebarVisible = true;

    updateControlsPosition();
    LOG_INFO("Exiting full screen mode");
}

void VideoWidget::onFullScreenClicked()
{
    enterFullScreen();
}

void VideoWidget::onExitFullScreenClicked()
{
    exitFullScreen();
}

void VideoWidget::checkMousePosition()
{
    QPoint globalMousePos = QCursor::pos();
    QPoint localMousePos = mapFromGlobal(globalMousePos);

    bool mouseInWindow = (localMousePos.x() >= 0 && localMousePos.x() <= width() &&
                          localMousePos.y() >= 0 && localMousePos.y() <= height());

    if (mouseInWindow) {
        bool shouldShowSidebar = (localMousePos.x() >= 0 && localMousePos.x() <= SIDEBAR_TRIGGER_ZONE);
        bool mouseInSidebar = (localMousePos.x() >= 0 && localMousePos.x() <= SIDEBAR_WIDTH &&
                               localMousePos.y() >= sidebar->y() &&
                               localMousePos.y() <= sidebar->y() + sidebar->height());

        if (shouldShowSidebar || mouseInSidebar) {
            if (!sidebarVisible) {
                showSidebar();
            }
            hideTimer->stop();
        } else if (sidebarVisible) {
            if (!hideTimer->isActive()) {
                hideTimer->start();
            }
        }
    } else if (sidebarVisible) {
        if (!hideTimer->isActive()) {
            hideTimer->start();
        }
    }
}

void VideoWidget::showSidebar()
{
    if (sidebarVisible) return;

    sidebarVisible = true;
    sidebar->setVisible(true);

    int sidebarHeight = height() * 0.4;
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebarAnimation->stop();
    sidebarAnimation->setStartValue(QPoint(-SIDEBAR_WIDTH + 3, sidebarY));
    sidebarAnimation->setEndValue(QPoint(0, sidebarY));
    sidebarAnimation->start();
}

void VideoWidget::hideSidebar()
{
    if (!sidebarVisible) return;

    sidebarVisible = false;

    int sidebarHeight = height() * 0.4;
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebarAnimation->stop();
    sidebarAnimation->setStartValue(QPoint(0, sidebarY));
    sidebarAnimation->setEndValue(QPoint(-SIDEBAR_WIDTH + 3, sidebarY));

    disconnect(sidebarAnimation, &QPropertyAnimation::finished, nullptr, nullptr);
    connect(sidebarAnimation, &QPropertyAnimation::finished, this, [this]() {
        if (!sidebarVisible) {
            sidebar->setVisible(false);
        }
    });
    sidebarAnimation->start();
}

void VideoWidget::enterEvent(QEnterEvent* event)
{
    QRhiWidget::enterEvent(event);
}

void VideoWidget::leaveEvent(QEvent* event)
{
    QRhiWidget::leaveEvent(event);
    if (sidebarVisible && isFullScreenMode) {
        if (!hideTimer->isActive()) {
            hideTimer->start();
        }
    }

    SystemParametersInfo(SPI_SETCURSORS, 0, NULL, 0);
}

}

}
