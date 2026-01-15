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
    createTextures(); // 这里面会创建 YUV 三个纹理
    createSampler();
    createShaderResourceBindings(); // 这里面会绑定三个纹理
    createPipeline();

    if (cb && rhi) {
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

        // 顶点数据不变
        static const float vertexData[] = {
            -1.0f,  1.0f,  0.0f, 0.0f,
            -1.0f, -1.0f,  0.0f, 1.0f,
            1.0f, -1.0f,  1.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
            1.0f, -1.0f,  1.0f, 1.0f,
            1.0f,  1.0f,  1.0f, 0.0f
        };
        batch->uploadStaticBuffer(vertexBuffer.get(), vertexData);

        // 初始化 Uniform，此时我们不需要 flag 区分 RGB/YUV 了，默认全走 YUV
        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(hasVideo ? 1.0f : 0.0f, 0.0f, 1.0f, 0.0f);

        for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            batch->updateDynamicBuffer(uniformBuffers[i].get(), 0, sizeof(UniformData), &uniformData);
            lastUniformData[i] = uniformData;
        }

        cb->resourceUpdate(batch);
    }
    return (pipeline != nullptr);
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
    if (!rhi) return;

    // 初始大小，使用 MAX 以避免频繁分配
    QSize sizeY(MAX_TEXTURE_WIDTH, MAX_TEXTURE_HEIGHT);
    QSize sizeUV(MAX_TEXTURE_WIDTH / 2, MAX_TEXTURE_HEIGHT / 2);

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        // 修改：移除所有标志位，使用默认值 (0)
        // 默认创建的纹理就可以被 Shader 采样，也可以通过 uploadTexture 上传数据

        // 创建 Y 纹理 (R8 格式，单通道)
        videoTexturesY[i].reset(rhi->newTexture(
            QRhiTexture::R8, sizeY, 1)); // 这里的 flags 默认为 QRhiTexture::Flags()
        videoTexturesY[i]->create();

        // 创建 U 纹理 (R8 格式，尺寸减半)
        videoTexturesU[i].reset(rhi->newTexture(
            QRhiTexture::R8, sizeUV, 1));
        videoTexturesU[i]->create();

        // 创建 V 纹理 (R8 格式，尺寸减半)
        videoTexturesV[i].reset(rhi->newTexture(
            QRhiTexture::R8, sizeUV, 1));
        videoTexturesV[i]->create();
    }
    LOG_INFO("YUV textures created");
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
    if (!rhi || !uniformBuffers[0] || !videoTexturesY[0] || !sampler) return;

    // 只需要创建 perFrameSrb，主 srb 如果没用到可以忽略
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        perFrameSrb[i].reset(rhi->newShaderResourceBindings());

        // 绑定顺序必须和 Shader 里的 binding 对应
        // binding 0: Uniform
        // binding 1: Texture Y
        // binding 2: Texture U
        // binding 3: Texture V
        perFrameSrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                uniformBuffers[i].get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                videoTexturesY[i].get(), sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                videoTexturesU[i].get(), sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                3, QRhiShaderResourceBinding::FragmentStage,
                videoTexturesV[i].get(), sampler.get())
        });

        perFrameSrb[i]->create();
    }
}

void VideoWidget::createPipeline()
{
    if (!rhi) {
        LOG_ERROR("createPipeline: RHI is null");
        return;
    }

    // 修改：Pipeline 需要一个 SRB 来确定布局（Binding Layout）。
    // 因为我们已经在 createShaderResourceBindings 里创建了 perFrameSrb，
    // 它们的布局都是一样的（Uniform + 3 Textures），所以直接用第 0 个作为模板即可。
    if (!perFrameSrb[0]) {
        LOG_ERROR("Cannot create pipeline: per-frame shader resource bindings not ready");
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

    // 修改：直接设置 perFrameSrb[0]
    // Pipeline 创建时会读取这个 SRB 的布局信息并“烘焙”进去。
    // 渲染时只要传具有相同布局的 SRB (perFrameSrb[0], [1], [2]) 都可以。
    pipeline->setShaderResourceBindings(perFrameSrb[0].get());

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
    if (!videoTexturesY[slot]) return true;
    QSize currentSize = videoTexturesY[slot]->pixelSize();

    // 如果视频尺寸变大了，必须重建
    if (width > currentSize.width() || height > currentSize.height()) return true;

    // 如果视频尺寸变小太多，也可以重建以节省显存
    if (std::abs(currentSize.width() - width) > MIN_TEXTURE_RESIZE_THRESHOLD) return true;

    return false;
}

void VideoWidget::recreateTextures(int slot, const QSize& newSize)
{
    QSize sizeY = newSize;
    QSize sizeUV(newSize.width() / 2, newSize.height() / 2);

    // 修改：移除 Sampled 标志，使用默认构造

    // 重建 Y
    videoTexturesY[slot].reset(rhi->newTexture(
        QRhiTexture::R8, sizeY, 1));
    videoTexturesY[slot]->create();

    // 重建 U
    videoTexturesU[slot].reset(rhi->newTexture(
        QRhiTexture::R8, sizeUV, 1));
    videoTexturesU[slot]->create();

    // 重建 V
    videoTexturesV[slot].reset(rhi->newTexture(
        QRhiTexture::R8, sizeUV, 1));
    videoTexturesV[slot]->create();

    // 重建 SRB
    perFrameSrb[slot].reset(rhi->newShaderResourceBindings());
    perFrameSrb[slot]->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffers[slot].get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            videoTexturesY[slot].get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage,
            videoTexturesU[slot].get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage,
            videoTexturesV[slot].get(), sampler.get())
    });
    perFrameSrb[slot]->create();

    LOG_INFO("Textures resized to %dx%d", newSize.width(), newSize.height());
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

    // 查找最新帧
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        int slotIndex = (currentFrameSlot.load(std::memory_order_acquire) - i + FRAME_BUFFER_COUNT) % FRAME_BUFFER_COUNT;
        if (frameBuffers[slotIndex].ready && frameBuffers[slotIndex].data) {
            frameToRender = slotIndex;
            break;
        }
    }

    if (frameToRender >= 0) {
        batch = rhi->nextResourceUpdateBatch();
        FrameBuffer& frame = frameBuffers[frameToRender];

        if (frame.needsUpdate && frame.ready && frame.data) {
            // 检查尺寸
            if (needsTextureResize(frame.width, frame.height, frameToRender)) {
                QSize newSize(std::min(frame.width, MAX_TEXTURE_WIDTH),
                              std::min(frame.height, MAX_TEXTURE_HEIGHT));
                recreateTextures(frameToRender, newSize);
            }

            // 确保纹理对象存在
            if (videoTexturesY[frameToRender]) {
                int w = frame.width;
                int h = frame.height;
                int w2 = (w + 1) / 2;
                int h2 = (h + 1) / 2;

                // 计算数据偏移量
                // 数据布局: [YYYY...][UU...][VV...]
                const uint8_t* dataY = frame.data.get();
                const uint8_t* dataU = dataY + (w * h);
                const uint8_t* dataV = dataU + (w2 * h2);

                // 上传 Y 平面
                QRhiTextureSubresourceUploadDescription subDescY(dataY, w * h);
                subDescY.setSourceSize(QSize(w, h));
                QRhiTextureUploadDescription descY({ 0, 0, subDescY });
                batch->uploadTexture(videoTexturesY[frameToRender].get(), descY);

                // 上传 U 平面
                QRhiTextureSubresourceUploadDescription subDescU(dataU, w2 * h2);
                subDescU.setSourceSize(QSize(w2, h2));
                QRhiTextureUploadDescription descU({ 0, 0, subDescU });
                batch->uploadTexture(videoTexturesU[frameToRender].get(), descU);

                // 上传 V 平面
                QRhiTextureSubresourceUploadDescription subDescV(dataV, w2 * h2);
                subDescV.setSourceSize(QSize(w2, h2));
                QRhiTextureUploadDescription descV({ 0, 0, subDescV });
                batch->uploadTexture(videoTexturesV[frameToRender].get(), descV);

                frame.needsUpdate = false;
                hasVideo = true;
                videoWidth = w;
                videoHeight = h;
            }
        }

        // 更新 Uniform
        if (frameToRender >= 0) {
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
        videoTexturesY[i].reset();
        videoTexturesU[i].reset();
        videoTexturesV[i].reset();
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
