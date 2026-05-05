#include "VideoWidget.h"
#include "WebRTCManager.h"
#include <QVBoxLayout>
#include <QFile>
#include <QCursor>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QDir>
#include <QWindow>
#include <QMetaObject>
#include <algorithm>
#include "Utils.h"

namespace hope {
namespace rtc {

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
    , hideTimer(nullptr)
    , sidebarAnimation(nullptr)
    , isFullScreenMode(false)
    , sidebarVisible(false)
    , interceptionHook(nullptr)
{
    qputenv("QSG_RENDER_LOOP", "basic");
    qputenv("QT_QSG_NO_VSYNC", "1");

    LOG_INFO("VideoWidget init (Dynamic texture mode)");

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

    lastUniformData.mvp.setToIdentity();
    lastUniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
    lastUniformData.uvScale = QVector2D(1.0f, 1.0f);

    initializeControls();
    LOG_INFO("VideoWidget init finished");
}

VideoWidget::~VideoWidget()
{
    LOG_INFO("VideoWidget destruction");
    VideoFrame* f1 = currentFramePtr.exchange(nullptr);
    if (f1) delete f1;
    VideoFrame* f2 = frameToReleasePtr.exchange(nullptr);
    if (f2) delete f2;
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
    createSampler();

    // 纹理将在收到第一帧时创建，这里先不创建
    // 但 pipeline 需要 SRB，而 SRB 需要纹理，因此我们用占位逻辑：
    // 创建一个 2x2 的占位纹理，保证 pipeline 能首次创建
    createTextures(2, 2);
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
        uniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
        uniformData.uvScale = QVector2D(1.0f, 1.0f);
        batch->updateDynamicBuffer(uniformBuffer.get(), 0, sizeof(UniformData), &uniformData);
        lastUniformData = uniformData;

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

    uniformBuffer.reset(rhi->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformData)));
    uniformBuffer->create();
}

void VideoWidget::createTextures(int width, int height)
{
    if (!rhi) return;

    LOG_INFO("Creating YUV textures: %dx%d", width, height);

    videoTextureY.reset(rhi->newTexture(QRhiTexture::R8, QSize(width, height), 1));
    videoTextureY->create();

    int chromaWidth = (width + 1) / 2;
    int chromaHeight = (height + 1) / 2;
    videoTextureU.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    videoTextureU->create();

    videoTextureV.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    videoTextureV->create();

    texWidth = width;
    texHeight = height;
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
    if (!rhi || !uniformBuffer || !videoTextureY || !sampler) return;

    srb.reset(rhi->newShaderResourceBindings());
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffer.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            videoTextureY.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage,
            videoTextureU.get(), sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage,
            videoTextureV.get(), sampler.get())
    });
    srb->create();
}

void VideoWidget::createPipeline()
{
    if (!rhi || !srb) return;

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
    pipeline->setShaderResourceBindings(srb.get());
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
    VideoFrame* f = currentFramePtr.exchange(nullptr);
    if (f) delete f;
    update();
}

void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame || !frame->buffer) return;

    // 强制切换到 GUI 线程，确保窗口操作线程安全
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this, frame]() {
            this->displayFrame(frame);
        }, Qt::QueuedConnection);
        return;
    }

    VideoFrame* newFrame = new VideoFrame(*frame);
    VideoFrame* oldFrame = currentFramePtr.exchange(newFrame, std::memory_order_acq_rel);
    if (oldFrame) delete oldFrame;

    VideoFrame* toRelease = frameToReleasePtr.exchange(nullptr, std::memory_order_acq_rel);
    if (toRelease) delete toRelease;

    if (QWindow* w = windowHandle()) {
        w->requestUpdate();
    } else {
        QMetaObject::invokeMethod(this, [this]() { this->update(); }, Qt::QueuedConnection);
    }
}

void VideoWidget::ensureTexturesForSize(int width, int height)
{
    if (width == texWidth && height == texHeight && videoTextureY && videoTextureU && videoTextureV)
        return; // 尺寸未变，纹理有效

    // 销毁旧纹理并重建，同时重建 SRB/pipeline（因为纹理绑定变了）
    videoTextureY.reset();
    videoTextureU.reset();
    videoTextureV.reset();

    createTextures(width, height);
    createShaderResourceBindings(); // SRB 需要重新绑定新纹理
    createPipeline();               // pipeline 依赖于 SRB
}

void VideoWidget::render(QRhiCommandBuffer* cb)
{
    if (!rhi || !resourcesInitialized || !pipeline || !renderTarget()) {
        if (renderTarget()) {
            const QColor clearColor(32, 32, 32);
            cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
            cb->endPass();
        }
        return;
    }

    VideoFrame* frameToRender = currentFramePtr.exchange(nullptr, std::memory_order_acq_rel);

    if (frameToRender && frameToRender->buffer) {
        auto* i420 = frameToRender->buffer.get();
        int srcWidth = i420->width();
        int srcHeight = i420->height();

        // 动态调整纹理尺寸
        ensureTexturesForSize(srcWidth, srcHeight);

        if (videoTextureY && videoTextureU && videoTextureV) {
            QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

            // Y 平面
            QRhiTextureSubresourceUploadDescription subDescY(
                i420->DataY(), i420->StrideY() * srcHeight);
            subDescY.setSourceSize(QSize(srcWidth, srcHeight));
            subDescY.setDataStride(i420->StrideY());
            batch->uploadTexture(videoTextureY.get(), QRhiTextureUploadDescription{{0, 0, subDescY}});

            int chromaW = (srcWidth + 1) / 2;
            int chromaH = (srcHeight + 1) / 2;
            QRhiTextureSubresourceUploadDescription subDescU(
                i420->DataU(), i420->StrideU() * chromaH);
            subDescU.setSourceSize(QSize(chromaW, chromaH));
            subDescU.setDataStride(i420->StrideU());
            batch->uploadTexture(videoTextureU.get(), QRhiTextureUploadDescription{{0, 0, subDescU}});

            QRhiTextureSubresourceUploadDescription subDescV(
                i420->DataV(), i420->StrideV() * chromaH);
            subDescV.setSourceSize(QSize(chromaW, chromaH));
            subDescV.setDataStride(i420->StrideV());
            batch->uploadTexture(videoTextureV.get(), QRhiTextureUploadDescription{{0, 0, subDescV}});

            // UV Scale 固定 (1,1)，因为纹理尺寸＝帧尺寸
            UniformData uniformData;
            uniformData.mvp.setToIdentity();
            uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f);
            uniformData.uvScale = QVector2D(1.0f, 1.0f);
            if (uniformData != lastUniformData) {
                batch->updateDynamicBuffer(uniformBuffer.get(), 0, sizeof(UniformData), &uniformData);
                lastUniformData = uniformData;
            }

            cb->resourceUpdate(batch);

            hasVideo = true;
            videoWidth = srcWidth;
            videoHeight = srcHeight;
            frameCount++;
        }
    }

    // 绘制
    const QColor clearColor = hasVideo ? Qt::black : QColor(48, 48, 48);
    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, nullptr);

    if (hasVideo && srb && pipeline) {
        const QSize outputSize = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(pipeline.get());
        cb->setViewport(QRhiViewport{0.0f, 0.0f, float(outputSize.width()), float(outputSize.height()), 0.0f, 1.0f});
        cb->setShaderResources(srb.get());
        const QRhiCommandBuffer::VertexInput vbufBinding(vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(6);
    }
    cb->endPass();

    if (frameToRender) {
        VideoFrame* oldRelease = frameToReleasePtr.exchange(frameToRender, std::memory_order_acq_rel);
        if (oldRelease) delete oldRelease;
    }
}

void VideoWidget::releaseResources()
{
    pipeline.reset();
    srb.reset();
    uniformBuffer.reset();
    videoTextureY.reset();
    videoTextureU.reset();
    videoTextureV.reset();
    sampler.reset();
    vertexBuffer.reset();
    resourcesInitialized = false;
    texWidth = texHeight = 0;
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

void VideoWidget::closeEvent(QCloseEvent *event)
{
    Q_EMIT disConnectRemote();
    QRhiWidget::closeEvent(event);
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

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    hideTimer->setInterval(HIDE_DELAY);
    connect(hideTimer, &QTimer::timeout, this, &VideoWidget::hideSidebar);

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

// 采用事件驱动机制响应鼠标移动，替代高频定时器
void VideoWidget::mouseMoveEvent(QMouseEvent* event)
{
    QRhiWidget::mouseMoveEvent(event);

    QPoint localMousePos = event->pos();

    // 判断鼠标是否处于触发区域
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

} // namespace rtc
} // namespace hope
