#include "videowidget.h"
#include "WebRTCManager.h"
#include <QVBoxLayout>
#include <QFile>
#include <QCursor>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QDir>
#include <QWindow>
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
    , interceptionHook(nullptr)
{
    // 强制关闭 VSync 以获得最低延迟（注意：可能会有画面撕裂，但延迟最低）
    qputenv("QSG_RENDER_LOOP", "basic");
    qputenv("QT_QSG_NO_VSYNC", "1");

    LOG_INFO("VideoWidget init (Low Latency Mode)");

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

    // 初始化 Uniform
    lastUniformData.mvp.setToIdentity();
    lastUniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
    lastUniformData.uvScale = QVector2D(1.0f, 1.0f);

    initializeControls();

    LOG_INFO("VideoWidget init finished");
}

VideoWidget::~VideoWidget()
{
    LOG_INFO("VideoWidget destruction");
    // 析构前清空引用
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        currentFrame.reset();
    }
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

void VideoWidget::createTextures()
{
    if (!rhi) return;

    QSize sizeY = MAX_TEXTURE_SIZE;
    QSize sizeUV(MAX_TEXTURE_SIZE.width() / 2, MAX_TEXTURE_SIZE.height() / 2);

    LOG_INFO("Allocating fixed YUV textures: %dx%d", sizeY.width(), sizeY.height());

    // 创建单套纹理
    videoTextureY.reset(rhi->newTexture(QRhiTexture::R8, sizeY, 1));
    videoTextureY->create();

    videoTextureU.reset(rhi->newTexture(QRhiTexture::R8, sizeUV, 1));
    videoTextureU->create();

    videoTextureV.reset(rhi->newTexture(QRhiTexture::R8, sizeUV, 1));
    videoTextureV->create();
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
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        currentFrame.reset();
    }
    update();
}

// --- 极低延迟核心：Mailbox 模式 ---
void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame || !frame->buffer) return;

    {
        // 加锁，直接覆盖当前帧，不管上一帧有没有被渲染
        // 这就是去掉了所有缓冲队列，只保留“最新”的一帧
        std::lock_guard<std::mutex> lock(frameMutex);
        currentFrame = frame;
    }

    if (QWindow* w = windowHandle())
        w->requestUpdate();
}

// --- 渲染线程 ---
void VideoWidget::render(QRhiCommandBuffer* cb)
{
    if (!rhi || !resourcesInitialized || !pipeline) {
        const QColor clearColor(32, 32, 32);
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    std::shared_ptr<VideoFrame> frameToRender = nullptr;

    // 1. 快速取出当前帧的引用，减少锁占用时间
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        frameToRender = currentFrame;
    }

    QVector2D currentUVScale(1.0f, 1.0f);

    if (frameToRender && frameToRender->buffer) {
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

        // 计算 UV Scale
        if (frameToRender->width > 0 && frameToRender->height > 0) {
            float scaleX = float(frameToRender->width) / MAX_TEXTURE_SIZE.width();
            float scaleY = float(frameToRender->height) / MAX_TEXTURE_SIZE.height();
            currentUVScale = QVector2D(scaleX, scaleY);
        } else {
            currentUVScale = QVector2D(0.0f, 0.0f);
        }

        auto* i420 = frameToRender->buffer.get();

        // 2. 上传 YUV 数据
        if (videoTextureY) {
            QRhiTextureSubresourceUploadDescription subDescY(i420->DataY(), i420->StrideY() * i420->height());
            subDescY.setSourceSize(QSize(frameToRender->width, frameToRender->height));
            subDescY.setDataStride(i420->StrideY());
            batch->uploadTexture(videoTextureY.get(), QRhiTextureUploadDescription{{0, 0, subDescY}});

            int chromaW = (frameToRender->width + 1) / 2;
            int chromaH = (frameToRender->height + 1) / 2;

            QRhiTextureSubresourceUploadDescription subDescU(i420->DataU(), i420->StrideU() * chromaH);
            subDescU.setSourceSize(QSize(chromaW, chromaH));
            subDescU.setDataStride(i420->StrideU());
            batch->uploadTexture(videoTextureU.get(), QRhiTextureUploadDescription{{0, 0, subDescU}});

            QRhiTextureSubresourceUploadDescription subDescV(i420->DataV(), i420->StrideV() * chromaH);
            subDescV.setSourceSize(QSize(chromaW, chromaH));
            subDescV.setDataStride(i420->StrideV());
            batch->uploadTexture(videoTextureV.get(), QRhiTextureUploadDescription{{0, 0, subDescV}});

            hasVideo = true;
            videoWidth = frameToRender->width;
            videoHeight = frameToRender->height;
        }

        // 3. 更新 Uniform
        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f);
        uniformData.uvScale = currentUVScale;

        if (uniformData != lastUniformData) {
            batch->updateDynamicBuffer(uniformBuffer.get(), 0, sizeof(UniformData), &uniformData);
            lastUniformData = uniformData;
        }

        cb->resourceUpdate(batch);
        frameCount++; // 统计FPS
    }

    const QColor clearColor = hasVideo ? Qt::black : QColor(48, 48, 48);
    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, nullptr);

    // 4. 绘制
    if (hasVideo && srb) {
        const QSize outputSize = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(pipeline.get());
        cb->setViewport(QRhiViewport{0.0f, 0.0f, float(outputSize.width()), float(outputSize.height()), 0.0f, 1.0f});
        cb->setShaderResources(srb.get());
        const QRhiCommandBuffer::VertexInput vbufBinding(vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(6);
    }
    cb->endPass();
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
