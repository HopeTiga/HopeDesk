#include "videowidget.h"
#include "WebRTCManager.h"
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
    // --- 优化 7: 关闭 VSync ---
    // 通过环境变量强制 Qt Quick/RHI 关闭垂直同步
    qputenv("QSG_RENDER_LOOP", "basic");
    qputenv("QT_QSG_NO_VSYNC", "1");

    LOG_INFO("VideoWidget init (VSync Disabled via Env)");

    QIcon windowIcon(":/logo/res/hope.png");
    if (!windowIcon.isNull()) {
        setWindowIcon(windowIcon);
    }

    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    fpsTimer.start();
    QTimer* fpsUpdateTimer = new QTimer(this);
    connect(fpsUpdateTimer, &QTimer::timeout, this, &VideoWidget::updateFPS);
    fpsUpdateTimer->start(1000);

    lastUpdateTime = std::chrono::steady_clock::now();

    for (auto& uniformData : lastUniformData) {
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
        uniformData.uvScale = QVector2D(1.0f, 1.0f);
    }

    for (auto& fb : frameBuffers) {
        fb.buffer = nullptr;   // 确保初始是空
        fb.width  = 0;
        fb.height = 0;
    }
    producerIdx.store(-1);
    consumerIdx.store(-1);

    initializeControls();

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

        // --- 优化 8: 加载 Pipeline Cache ---
        loadPipelineCache();

        resourcesInitialized = false;
    }

    if (!resourcesInitialized) {
        LOG_INFO("Starting video rendering resource initialization");
        if (!initializeResources(cb)) {
            LOG_ERROR("Resource initialization failed");
            return;
        }
        resourcesInitialized = true;
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

        static const float vertexData[] = {
            -1.0f,  1.0f,  0.0f, 0.0f,
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 0.0f
        };
        batch->uploadStaticBuffer(vertexBuffer.get(), vertexData);

        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(hasVideo ? 1.0f : 0.0f, 0.0f, 1.0f, 0.0f);
        uniformData.uvScale = QVector2D(1.0f, 1.0f);

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
    if (!rhi) return;
    vertexBuffer.reset(rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 6 * 4 * sizeof(float)));
    vertexBuffer->create();

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        uniformBuffers[i].reset(rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformData)));
        uniformBuffers[i]->create();
    }
}

void VideoWidget::createTextures()
{
    if (!rhi) return;

    QSize sizeY = MAX_TEXTURE_SIZE;
    QSize sizeUV(MAX_TEXTURE_SIZE.width() / 2, MAX_TEXTURE_SIZE.height() / 2);

    LOG_INFO("Allocating fixed YUV textures: %dx%d", sizeY.width(), sizeY.height());

    // --- 修复编译错误 2: 移除 UsedAsTransferDestination ---
    // QRhi 纹理默认支持 upload，无需特殊 flag (除非是 RenderTarget)
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        videoTexturesY[i].reset(rhi->newTexture(QRhiTexture::R8, sizeY, 1));
        videoTexturesY[i]->create();

        videoTexturesU[i].reset(rhi->newTexture(QRhiTexture::R8, sizeUV, 1));
        videoTexturesU[i]->create();

        videoTexturesV[i].reset(rhi->newTexture(QRhiTexture::R8, sizeUV, 1));
        videoTexturesV[i]->create();
    }
}

void VideoWidget::createSampler()
{
    if (!rhi) return;
    sampler.reset(rhi->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler->create();
}

void VideoWidget::createShaderResourceBindings()
{
    if (!rhi || !uniformBuffers[0] || !videoTexturesY[0] || !sampler) return;

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        perFrameSrb[i].reset(rhi->newShaderResourceBindings());
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
    if (!rhi || !perFrameSrb[0]) return;

    pipeline.reset(rhi->newGraphicsPipeline());

    QShader vertShader = getShader(":/shaders/res/video.vert.qsb");
    QShader fragShader = getShader(":/shaders/res/video.frag.qsb");

    if (!vertShader.isValid() || !fragShader.isValid()) {
        LOG_ERROR("Invalid shaders");
        return;
    }

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vertShader },
        { QRhiShaderStage::Fragment, fragShader }
    });

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ { 4 * sizeof(float) } });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) }
    });

    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(perFrameSrb[0].get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    pipeline->setDepthTest(false);
    pipeline->setDepthWrite(false);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->create();
}

void VideoWidget::clearDisplay()
{
    LOG_INFO("Clearing display");
    hasVideo = false;
    // 重置索引
    producerIdx.store(-1);
    consumerIdx.store(-1);
    // 释放引用
    for (auto& buffer : frameBuffers) {
        buffer.buffer = nullptr;
    }
    update();
}

// --- 优化 9: 生产者 (无锁逻辑) ---
void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame || !frame->buffer) return;

    int cIdx = consumerIdx.load(std::memory_order_acquire);
    int pIdx = producerIdx.load(std::memory_order_relaxed);
    int nextIdx = 0;
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        if (i != cIdx && i != pIdx) { nextIdx = i; break; }
    }
    // 关键：先清空旧引用再写新引用，防止野指针
    frameBuffers[nextIdx].buffer = nullptr;   // ★释放旧帧
    frameBuffers[nextIdx].buffer = frame->buffer;
    frameBuffers[nextIdx].width  = frame->width;
    frameBuffers[nextIdx].height = frame->height;

    producerIdx.store(nextIdx, std::memory_order_release);
    update();

}

// --- 优化 9: 消费者 (无锁逻辑) ---
void VideoWidget::render(QRhiCommandBuffer* cb)
{

    if (!rhi || !resourcesInitialized || !pipeline) {
        const QColor clearColor(32, 32, 32);
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    // 检查是否有新帧
    int pIdx = producerIdx.load(std::memory_order_acquire);
    int cIdx = consumerIdx.load(std::memory_order_relaxed);

    if (pIdx != -1 && pIdx != cIdx) {
        cIdx = pIdx;
        consumerIdx.store(cIdx, std::memory_order_release); // 锁定这个槽位用于显示
    }

    QVector2D currentUVScale(1.0f, 1.0f);

    int frameToRender = cIdx;

    if (frameToRender >= 0) {
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
        FrameBuffer& frame = frameBuffers[frameToRender];

        // 如果该帧有效
        if (frame.buffer) {
            // 计算 UV Scale
            if (frame.width > 0 && frame.height > 0) {
                float scaleX = float(frame.width) / MAX_TEXTURE_SIZE.width();
                float scaleY = float(frame.height) / MAX_TEXTURE_SIZE.height();
                currentUVScale = QVector2D(scaleX, scaleY);
            } else {
                currentUVScale = QVector2D(0.0f, 0.0f);
            }

            if (videoTexturesY[frameToRender]) {
                auto* i420 = frame.buffer.get();

                QRhiTextureSubresourceUploadDescription subDescY(i420->DataY(), i420->StrideY() * i420->height());
                subDescY.setSourceSize(QSize(frame.width, frame.height));
                subDescY.setDataStride(i420->StrideY());
                batch->uploadTexture(videoTexturesY[frameToRender].get(), QRhiTextureUploadDescription{{0, 0, subDescY}});

                int chromaW = (frame.width + 1) / 2;
                int chromaH = (frame.height + 1) / 2;

                QRhiTextureSubresourceUploadDescription subDescU(i420->DataU(), i420->StrideU() * chromaH);
                subDescU.setSourceSize(QSize(chromaW, chromaH));
                subDescU.setDataStride(i420->StrideU());
                batch->uploadTexture(videoTexturesU[frameToRender].get(), QRhiTextureUploadDescription{{0, 0, subDescU}});

                QRhiTextureSubresourceUploadDescription subDescV(i420->DataV(), i420->StrideV() * chromaH);
                subDescV.setSourceSize(QSize(chromaW, chromaH));
                subDescV.setDataStride(i420->StrideV());
                batch->uploadTexture(videoTexturesV[frameToRender].get(), QRhiTextureUploadDescription{{0, 0, subDescV}});

                hasVideo = true;
                videoWidth = frame.width;
                videoHeight = frame.height;
            }
        }

        // 更新 Uniform
        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f);
        uniformData.uvScale = currentUVScale;

        if (uniformData != lastUniformData[frameToRender]) {
            batch->updateDynamicBuffer(uniformBuffers[frameToRender].get(), 0, sizeof(UniformData), &uniformData);
            lastUniformData[frameToRender] = uniformData;
        }

        // 提交上传
        cb->resourceUpdate(batch);
    }

    const QColor clearColor = hasVideo ? Qt::black : QColor(48, 48, 48);
    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, nullptr); // batch 已经在上面提交了

    if (frameToRender >= 0 && perFrameSrb[frameToRender]) {
        const QSize outputSize = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(pipeline.get());
        cb->setViewport(QRhiViewport{0.0f, 0.0f, float(outputSize.width()), float(outputSize.height()), 0.0f, 1.0f});
        cb->setShaderResources(perFrameSrb[frameToRender].get());
        const QRhiCommandBuffer::VertexInput vbufBinding(vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(6);
        frameCount++;
    }
    cb->endPass();
}

void VideoWidget::releaseResources()
{
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
        return QShader::fromSerialized(f.readAll());
    }
    return QShader();
}

void VideoWidget::loadPipelineCache()
{
    if (!rhi) return;
    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/pipeline.cache";
    QFile cacheFile(cachePath);
    if (cacheFile.open(QIODevice::ReadOnly)) {
        QByteArray cacheData = cacheFile.readAll();
        if (!cacheData.isEmpty()) {
            rhi->setPipelineCacheData(cacheData);
            LOG_INFO("Pipeline cache loaded: %lld bytes", cacheData.size());
        }
    }
}

void VideoWidget::savePipelineCache()
{
    if (!rhi) return;
    QByteArray cacheData = rhi->pipelineCacheData();
    if (cacheData.isEmpty()) return;

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QString cachePath = cacheDir + "/pipeline.cache";
    QFile cacheFile(cachePath);
    if (cacheFile.open(QIODevice::WriteOnly)) {
        cacheFile.write(cacheData);
    }
}

void VideoWidget::updateFPS()
{
    qint64 elapsed = fpsTimer.elapsed();
    if (elapsed > 0) {
        currentFPS = (frameCount * 1000.0) / elapsed;
        frameCount = 0;
        fpsTimer.restart();
    }
}

// ... 辅助函数不变 ...
void VideoWidget::setWebRTCManager(WebRTCManager * manager)
{
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
