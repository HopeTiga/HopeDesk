#include "videowidget.h"
#include "webrtcremoteclient.h"
#include "Logger.h"
#include <QVBoxLayout>
#include <QFile>
#include <QCursor>
#include <QResizeEvent>
#include <algorithm>

VideoWidget::VideoWidget(QWidget* parent)
    : QRhiWidget(parent)
    , webRTCRemoteClient(nullptr)
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
{
    // 获取Logger实例
    logger = Logger::getInstance();
    logger->info("VideoWidget初始化开始");

    QIcon windowIcon(":/logo/res/Wilson_DST.png");
    if (!windowIcon.isNull()) {
        setWindowIcon(windowIcon);
    }

    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    // 初始化FPS计时器
    fpsTimer.start();
    QTimer* fpsUpdateTimer = new QTimer(this);
    connect(fpsUpdateTimer, &QTimer::timeout, this, &VideoWidget::updateFPS);
    fpsUpdateTimer->start(1000);

    // 初始化控件
    initializeControls();

    logger->info("VideoWidget初始化完成");
}

VideoWidget::~VideoWidget()
{
    logger->info("VideoWidget析构");
}

void VideoWidget::initialize(QRhiCommandBuffer* cb)
{
    if (!QRhiWidget::rhi()) {
        logger->error("RHI未初始化");
        return;
    }

    // 保存RHI实例
    if (rhi != QRhiWidget::rhi()) {
        logger->info("RHI实例改变，重新创建资源");
        releaseResources();
        rhi = QRhiWidget::rhi();
        resourcesInitialized = false;
    }

    if (!resourcesInitialized) {
        logger->info("开始初始化视频渲染资源");

        if (!initializeResources(cb)) {
            logger->error("初始化资源失败");
            return;
        }

        resourcesInitialized = true;
        logger->info("视频渲染资源初始化完成");
    }
}

bool VideoWidget::initializeResources(QRhiCommandBuffer* cb)
{
    // 1. 创建顶点缓冲
    createBuffers();

    // 2. 创建纹理
    createTextures();

    // 3. 创建采样器
    createSampler();

    // 4. 创建着色器资源绑定
    createShaderResourceBindings();

    // 5. 创建管线
    createPipeline();

    // 6. 上传初始数据
    if (cb && rhi) {
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();

        // 上传顶点数据
        static const float vertexData[] = {
            // 位置         // 纹理坐标
            -1.0f,  1.0f,  0.0f, 0.0f,  // 左上
            -1.0f, -1.0f,  0.0f, 1.0f,  // 左下
            1.0f, -1.0f,  1.0f, 1.0f,  // 右下
            -1.0f,  1.0f,  0.0f, 0.0f,  // 左上
            1.0f, -1.0f,  1.0f, 1.0f,  // 右下
            1.0f,  1.0f,  1.0f, 0.0f   // 右上
        };
        batch->uploadStaticBuffer(vertexBuffer.get(), vertexData);

        // 初始化uniform buffers
        UniformData uniformData;
        uniformData.mvp.setToIdentity();
        uniformData.params = QVector4D(
            hasVideo ? 1.0f : 0.0f,  // x: hasVideo
            0.0f,                     // y: isYUV
            1.0f,                     // z: brightness
            0.0f                      // w: padding
            );

        for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            batch->updateDynamicBuffer(uniformBuffers[i].get(), 0, sizeof(UniformData), &uniformData);
        }

        cb->resourceUpdate(batch);
    }

    bool success = (pipeline != nullptr);
    if (!success) {
        logger->error("initializeResources失败");
    }
    return success;
}

void VideoWidget::createBuffers()
{
    if (!rhi) {
        logger->error("createBuffers: RHI为空");
        return;
    }

    // 创建顶点缓冲
    vertexBuffer.reset(rhi->newBuffer(
        QRhiBuffer::Immutable,
        QRhiBuffer::VertexBuffer,
        6 * 4 * sizeof(float)
        ));

    if (!vertexBuffer || !vertexBuffer->create()) {
        logger->error("顶点缓冲创建失败");
        return;
    }

    // 创建uniform缓冲
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        uniformBuffers[i].reset(rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            sizeof(UniformData)
            ));

        if (!uniformBuffers[i] || !uniformBuffers[i]->create()) {
            logger->error(std::string("uniform缓冲") + std::to_string(i) + "创建失败");
            return;
        }
    }
}

void VideoWidget::createTextures()
{
    if (!rhi) {
        logger->error("createTextures: RHI为空");
        return;
    }

    // 使用更大的初始尺寸，避免后续频繁重建纹理
    QSize initialSize(std::max(videoWidth.load(), 1920), std::max(videoHeight.load(), 1080));

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        // 使用RGBA8格式
        videoTextures[i].reset(rhi->newTexture(
            QRhiTexture::RGBA8,
            initialSize,
            1,
            QRhiTexture::Flag(0)
            ));

        if (!videoTextures[i]) {
            logger->error(std::string("创建视频纹理对象") + std::to_string(i) + "失败");
            return;
        }

        if (!videoTextures[i]->create()) {
            logger->warning(std::string("创建视频纹理") + std::to_string(i) + "失败，尝试创建备用纹理");

            // 创建备用的较小纹理
            videoTextures[i].reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(640, 480)));
            if (!videoTextures[i] || !videoTextures[i]->create()) {
                logger->error(std::string("创建备用纹理") + std::to_string(i) + "也失败了");
                return;
            }
        }
    }
}

void VideoWidget::createSampler()
{
    if (!rhi) {
        logger->error("createSampler: RHI为空");
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
        logger->error("采样器创建失败");
        return;
    }
}

void VideoWidget::createShaderResourceBindings()
{
    if (!rhi) {
        logger->error("createShaderResourceBindings: RHI为空");
        return;
    }

    // 检查依赖资源
    if (!uniformBuffers[0] || !videoTextures[0] || !sampler) {
        logger->error("依赖资源未准备好");
        return;
    }

    // 创建主SRB
    srb.reset(rhi->newShaderResourceBindings());
    if (!srb) {
        logger->error("创建SRB对象失败");
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
        logger->error("主着色器资源绑定create()失败");
        srb.reset();
        return;
    }

    // 为每个帧创建兼容的SRB
    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
        perFrameSrb[i].reset(rhi->newShaderResourceBindings());
        if (!perFrameSrb[i]) {
            logger->error(std::string("创建帧") + std::to_string(i) + "的SRB对象失败");
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
            logger->error(std::string("帧") + std::to_string(i) + "的着色器资源绑定create()失败");
            perFrameSrb[i].reset();
        }
    }
}

void VideoWidget::createPipeline()
{
    if (!rhi) {
        logger->error("createPipeline: RHI为空");
        return;
    }

    if (!srb) {
        logger->error("无法创建管线：着色器资源绑定未准备好");
        return;
    }

    pipeline.reset(rhi->newGraphicsPipeline());
    if (!pipeline) {
        logger->error("创建pipeline对象失败");
        return;
    }

    // 加载着色器
    QShader vertShader = getShader(":/shaders/res/video.vert.qsb");
    QShader fragShader = getShader(":/shaders/res/video.frag.qsb");

    if (!vertShader.isValid() || !fragShader.isValid()) {
        logger->error(std::string("着色器无效 - vertex: ") +
                      (vertShader.isValid() ? "有效" : "无效") +
                      ", fragment: " +
                      (fragShader.isValid() ? "有效" : "无效"));
        pipeline.reset();
        return;
    }

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vertShader },
        { QRhiShaderStage::Fragment, fragShader }
    });

    // 设置顶点输入布局
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        { 4 * sizeof(float) }  // 每个顶点4个float
    });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },                  // 位置
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) }   // 纹理坐标
    });

    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    pipeline->setDepthTest(false);
    pipeline->setDepthWrite(false);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);

    if (!pipeline->create()) {
        logger->error("渲染管线create()失败");
        pipeline.reset();
        return;
    }
}

void VideoWidget::render(QRhiCommandBuffer* cb) {
    if (!rhi || !resourcesInitialized || !pipeline) {
        const QColor clearColor(32, 32, 32);
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    QRhiResourceUpdateBatch* batch = nullptr;
    int frameToRender = -1;

    {
        for (int i = 0; i < FRAME_BUFFER_COUNT; ++i) {
            int slotIndex = (currentFrameSlot - i + FRAME_BUFFER_COUNT) % FRAME_BUFFER_COUNT;
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
            QSize currentTextureSize = videoTextures[frameToRender]->pixelSize();
            QSize frameSize(frame.width, frame.height);

            if (currentTextureSize != frameSize) {
                logger->info(std::string("视频尺寸变化: ") +
                             std::to_string(currentTextureSize.width()) + "x" +
                             std::to_string(currentTextureSize.height()) +
                             " -> " + std::to_string(frameSize.width()) + "x" +
                             std::to_string(frameSize.height()));

                videoTextures[frameToRender].reset(rhi->newTexture(
                    QRhiTexture::RGBA8,
                    frameSize,
                    1,
                    QRhiTexture::Flag(0)
                    ));

                if (videoTextures[frameToRender] && videoTextures[frameToRender]->create()) {
                    perFrameSrb[frameToRender].reset(rhi->newShaderResourceBindings());
                    perFrameSrb[frameToRender]->setBindings({
                        QRhiShaderResourceBinding::uniformBuffer(
                            0,
                            QRhiShaderResourceBinding::VertexStage |
                                QRhiShaderResourceBinding::FragmentStage,
                            uniformBuffers[frameToRender].get()
                            ),
                        QRhiShaderResourceBinding::sampledTexture(
                            1,
                            QRhiShaderResourceBinding::FragmentStage,
                            videoTextures[frameToRender].get(),
                            sampler.get()
                            )
                    });
                    perFrameSrb[frameToRender]->create();
                } else {
                    logger->error("重建纹理失败");
                    frameToRender = -1;
                }
            }

            if (frameToRender >= 0) {
                // 直接上传RGBA数据，无需转换！
                size_t rgbaSize = frame.width * frame.height * 4;

                QRhiTextureSubresourceUploadDescription subresDesc(
                    frame.data.get(),
                    rgbaSize
                    );
                QRhiTextureUploadDescription desc({ 0, 0, subresDesc });
                batch->uploadTexture(videoTextures[frameToRender].get(), desc);

                frame.needsUpdate = false;
                hasVideo = true;
                videoWidth = frame.width;
                videoHeight = frame.height;
            }
        }

        if (frameToRender >= 0) {
            UniformData uniformData;
            uniformData.mvp.setToIdentity();
            uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f);
            batch->updateDynamicBuffer(uniformBuffers[frameToRender].get(),
                                       0, sizeof(UniformData), &uniformData);
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

    // 使用轮转策略
    int nextSlot = (currentFrameSlot + 1) % FRAME_BUFFER_COUNT;

    {
        std::lock_guard<std::mutex> lock(frameMutex);

        // 设置新数据
        FrameBuffer& buffer = frameBuffers[nextSlot];
        buffer.data = frame->data;
        buffer.width = frame->width;
        buffer.height = frame->height;
        buffer.ready = true;
        buffer.needsUpdate = true;

        // 更新当前槽位
        currentFrameSlot = nextSlot;
    }

    update();
}

void VideoWidget::clearDisplay()
{
    logger->info("清除显示");
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
    logger->info("释放渲染资源");

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
            logger->error(std::string("着色器无效：") + name.toStdString());
        }
    } else {
        logger->error(std::string("无法打开着色器文件：") + name.toStdString());
    }

    return QShader();
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

void VideoWidget::setWebRTCRemoteClient(WebRTCRemoteClient* client)
{
    logger->info("设置WebRTC远程客户端");
    webRTCRemoteClient = client;

    windowsHook = std::make_unique<WindowsHook>();
    windowsHook->setTargetWidget(this);
    windowsHook->setRemoteClient(webRTCRemoteClient);
    windowsHook->setVideoSize(width(), height());
    windowsHook->startHook();
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
    if (windowsHook) {
        windowsHook->setVideoSize(event->size().width(), event->size().height());
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
    logger->info("进入全屏模式");
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
    logger->info("退出全屏模式");
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

    SystemParametersInfo(SPI_SETCURSORS,0,NULL,0);
}
