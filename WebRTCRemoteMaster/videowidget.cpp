#include "videowidget.h"
#include "webrtcremoteclient.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QScreen>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
// 为非Windows平台定义VK_*常量
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_F1 0x70
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#endif

// 顶点着色器 - 优化版本
const char* VideoWidget::vertexShaderSource = R"(
#version 450 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// 片段着色器 - 优化版本
const char* VideoWidget::fragmentShaderSource = R"(
#version 450 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D videoTexture;
uniform bool hasVideo;
uniform bool isYUV;

const mat3 yuv2rgb = mat3(
    1.164,  1.164, 1.164,
    0.0,   -0.392, 2.017,
    1.596, -0.813, 0.0
);

void main()
{
    if (!hasVideo) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    if (isYUV) {
        vec3 yuv = texture(videoTexture, TexCoord).rgb;
        yuv.x = yuv.x - 0.0625;
        yuv.yz = yuv.yz - 0.5;
        vec3 rgb = yuv2rgb * yuv;
        FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
    } else {
        FragColor = texture(videoTexture, TexCoord);
    }
}
)";

VideoWidget::VideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , shaderProgram(nullptr)
    , textureId(0)
    , videoWidth(0)
    , videoHeight(0)
    , frameCount(0)
    , currentFPS(0.0)
    , lastFPSUpdate(0)
    , hasVideo(false)
    , needsTextureUpdate(false)
    , textureCreated(false)
    , currentPboIndex(0)
    , uploadPboIndex(0)
    , currentFrameIndex(0)
    , readyFrameIndex(-1)
    , pboSize(0)
    , gpuAccelerationEnabled(true)
    , maxTextureSize(0)
    , uniformHasVideo(-1)
    , uniformVideoTexture(-1)
    , uniformIsYUV(-1)
    , fullScreenButton(nullptr)
    , exitFullScreenButton(nullptr)
    , sidebar(nullptr)
    , sidebarExitButton(nullptr)
    , mouseCheckTimer(nullptr)
    , hideTimer(nullptr)
    , sidebarAnimation(nullptr)
    , isFullScreenMode(false)
    , sidebarVisible(false)
{
    QIcon windowIcon(":/logo/res/Wilson_DST.png");
    if (!windowIcon.isNull()) {
        setWindowIcon(windowIcon);
        qDebug() << "视频窗口图标设置成功";
    } else {
        qDebug() << "警告：无法加载视频窗口图标：:/logo/res/Wilson_DST.png";
    }

    // 设置窗口属性 - 专注于OpenGL渲染
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    setMouseTracking(true);
    setAttribute(Qt::WA_AcceptTouchEvents);

    // 关闭Qt的自动填充和部分更新
    setAutoFillBackground(false);
    setUpdatesEnabled(true);

    // 设置OpenGL格式 - 优化GPU使用
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapBehavior(QSurfaceFormat::TripleBuffer); // 三重缓冲
    format.setSwapInterval(0); // 禁用VSync获得最高帧率，如需限制帧率可设为1
    format.setDepthBufferSize(0); // 不需要深度缓冲
    format.setStencilBufferSize(0); // 不需要模板缓冲
    format.setSamples(0); // 不需要多重采样
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);

    setFormat(format);

    // 初始化PBO ID
    std::fill(std::begin(pboIds), std::end(pboIds), 0);

    // 初始化帧缓冲
    for (int i = 0; i < 3; ++i) {
        frameBuffers[i].ready = false;
        frameBuffers[i].width = 0;
        frameBuffers[i].height = 0;
    }

    // 初始化FPS计时器
    fpsTimer.start();
    QTimer* fpsUpdateTimer = new QTimer(this);
    connect(fpsUpdateTimer, &QTimer::timeout, this, &VideoWidget::updateFPS);
    fpsUpdateTimer->start(1000);

    // 初始化控件
    initializeControls();
    initKeyMap();
}

VideoWidget::~VideoWidget()
{
    makeCurrent();

    // 清理OpenGL资源
    if (textureId) {
        glDeleteTextures(1, &textureId);
    }

    if (pboIds[0]) {
        glDeleteBuffers(PBO_COUNT, pboIds);
    }

    delete shaderProgram;

    doneCurrent();
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

    // 创建侧边栏布局
    QVBoxLayout* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(5, 10, 5, 10);  // 减小边距
    sidebarLayout->setSpacing(5);  // 减小间距

    // 全屏按钮（普通模式下在侧边栏中）
    fullScreenButton = new QPushButton("全\n屏", sidebar);  // 改为两行文字以适应较窄的宽度
    fullScreenButton->setFixedSize(20, 20);  // 减小按钮尺寸
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
            transform: scale(1.05);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #553C9A, stop:1 #5B21B6);
        }
    )");
    connect(fullScreenButton, &QPushButton::clicked, this, &VideoWidget::onFullScreenClicked);
    fullScreenButton->setVisible(true);  // 初始时可见

    // 退出全屏按钮（全屏模式下在侧边栏中）
    sidebarExitButton = new QPushButton("退出\n全屏", sidebar);
    sidebarExitButton->setFixedSize(20, 20);  // 减小按钮尺寸
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
            transform: scale(1.05);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #C53030, stop:1 #9C1C1C);
        }
    )");
    sidebarExitButton->setVisible(false);  // 初始时隐藏
    connect(sidebarExitButton, &QPushButton::clicked, this, &VideoWidget::onExitFullScreenClicked);

    // 将按钮添加到布局
    sidebarLayout->addWidget(fullScreenButton);
    sidebarLayout->addWidget(sidebarExitButton);
    sidebarLayout->addStretch();

    // 创建侧边栏动画
    sidebarAnimation = new QPropertyAnimation(sidebar, "pos", this);
    sidebarAnimation->setDuration(250);  // 稍微加快动画速度
    sidebarAnimation->setEasingCurve(QEasingCurve::OutCubic);

    // 创建定时器
    mouseCheckTimer = new QTimer(this);
    mouseCheckTimer->setInterval(50); // 每50ms检查一次鼠标位置
    connect(mouseCheckTimer, &QTimer::timeout, this, &VideoWidget::checkMousePosition);

    hideTimer = new QTimer(this);
    hideTimer->setSingleShot(true);
    hideTimer->setInterval(HIDE_DELAY);
    connect(hideTimer, &QTimer::timeout, this, &VideoWidget::hideSidebar);

    // 始终启动鼠标检查（用于显示侧边栏）
    mouseCheckTimer->start();

    // 初始显示侧边栏（普通模式）
    sidebar->setVisible(true);
    sidebarVisible = true;
    updateControlsPosition();
}

void VideoWidget::updateControlsPosition()
{
    if (!sidebar) return;

    int sidebarHeight = height() * 0.4; // 减小高度到40%
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebar->setFixedHeight(sidebarHeight);

    if (sidebarVisible) {
        sidebar->move(0, sidebarY);
    } else {
        sidebar->move(-SIDEBAR_WIDTH + 3, sidebarY); // 隐藏时只露出3px边缘
    }

    // 根据模式显示/隐藏按钮
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
    QOpenGLWidget::resizeEvent(event);
    updateControlsPosition();
}

void VideoWidget::enterFullScreen()
{
    if (isFullScreenMode) return;

    // 保存当前窗口状态
    normalGeometry = geometry();
    normalWindowState = windowState();

    // 设置全屏
    isFullScreenMode = true;
    setWindowState(Qt::WindowFullScreen);

    // 更新控件
    updateControlsPosition();

    qDebug() << "进入全屏模式";
}

void VideoWidget::exitFullScreen()
{
    if (!isFullScreenMode) return;

    // 隐藏侧边栏
    sidebar->setVisible(false);
    sidebarVisible = false;

    // 恢复窗口状态
    isFullScreenMode = false;
    setWindowState(normalWindowState);
    setGeometry(normalGeometry);

    // 重新显示侧边栏（普通模式下）
    sidebar->setVisible(true);
    sidebarVisible = true;

    // 更新控件
    updateControlsPosition();

    qDebug() << "退出全屏模式";
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

    // 检查鼠标是否在窗口内
    bool mouseInWindow = (localMousePos.x() >= 0 && localMousePos.x() <= width() &&
                          localMousePos.y() >= 0 && localMousePos.y() <= height());

    if (mouseInWindow) {
        // 检查鼠标是否在左边缘触发区域
        bool shouldShowSidebar = (localMousePos.x() >= 0 && localMousePos.x() <= SIDEBAR_TRIGGER_ZONE);

        // 检查鼠标是否在侧边栏内
        bool mouseInSidebar = (localMousePos.x() >= 0 && localMousePos.x() <= SIDEBAR_WIDTH &&
                               localMousePos.y() >= sidebar->y() &&
                               localMousePos.y() <= sidebar->y() + sidebar->height());

        if (shouldShowSidebar || mouseInSidebar) {
            // 显示侧边栏
            if (!sidebarVisible) {
                showSidebar();
            }
            // 如果鼠标在侧边栏内，停止隐藏定时器
            hideTimer->stop();
        } else if (sidebarVisible) {
            // 如果鼠标不在侧边栏区域，启动隐藏定时器
            if (!hideTimer->isActive()) {
                hideTimer->start();
            }
        }
    } else if (sidebarVisible) {
        // 如果鼠标移出窗口，启动隐藏定时器
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

    // 动画显示侧边栏
    int sidebarHeight = height() * 0.4;
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebarAnimation->stop();  // 停止之前的动画
    sidebarAnimation->setStartValue(QPoint(-SIDEBAR_WIDTH + 3, sidebarY));
    sidebarAnimation->setEndValue(QPoint(0, sidebarY));
    sidebarAnimation->start();

    qDebug() << "显示侧边栏";
}

void VideoWidget::hideSidebar()
{
    if (!sidebarVisible) return;

    sidebarVisible = false;

    // 动画隐藏侧边栏
    int sidebarHeight = height() * 0.4;
    int sidebarY = (height() - sidebarHeight) / 2;

    sidebarAnimation->stop();  // 停止之前的动画
    sidebarAnimation->setStartValue(QPoint(0, sidebarY));
    sidebarAnimation->setEndValue(QPoint(-SIDEBAR_WIDTH + 3, sidebarY));

    // 断开之前的连接，避免重复连接
    disconnect(sidebarAnimation, &QPropertyAnimation::finished, nullptr, nullptr);

    connect(sidebarAnimation, &QPropertyAnimation::finished, [this]() {
        if (!sidebarVisible) {  // 确保状态正确
            sidebar->setVisible(false);
        }
    });
    sidebarAnimation->start();

    qDebug() << "隐藏侧边栏";
}

// 添加鼠标进入和离开事件处理
void VideoWidget::enterEvent(QEnterEvent* event)
{
    QOpenGLWidget::enterEvent(event);
    // 鼠标进入窗口时不做特殊处理，由checkMousePosition处理
}

void VideoWidget::leaveEvent(QEvent* event)
{
    QOpenGLWidget::leaveEvent(event);
    // 鼠标离开窗口时，启动隐藏定时器
    if (sidebarVisible && isFullScreenMode) {
        if (!hideTimer->isActive()) {
            hideTimer->start();
        }
    }
}

// 以下是原有的OpenGL和事件处理代码，保持不变...

void VideoWidget::checkGLError(const char* operation)
{
#ifdef DEBUG
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        qDebug() << "OpenGL Error in" << operation << ":" << error;
    }
#endif
}

void VideoWidget::initializeGL()
{
    // 初始化OpenGL函数
    if (!initializeOpenGLFunctions()) {
        qDebug() << "Failed to initialize OpenGL functions";
        return;
    }

    // 打印GPU信息
    qDebug() << "OpenGL Version:" << (char*)glGetString(GL_VERSION);
    qDebug() << "GLSL Version:" << (char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    qDebug() << "GPU Vendor:" << (char*)glGetString(GL_VENDOR);
    qDebug() << "GPU Renderer:" << (char*)glGetString(GL_RENDERER);

    // 获取GPU能力
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    qDebug() << "Max Texture Size:" << maxTextureSize;

    // 设置OpenGL状态 - 优化GPU渲染
    glDisable(GL_DEPTH_TEST);  // 不需要深度测试
    glDisable(GL_BLEND);       // 不需要混合
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);

    // 设置清除颜色
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // 设置像素解包参数
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // 字节对齐
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    // 初始化着色器
    initializeShaders();

    // 初始化顶点数据
    initializeVertexData();

    // 创建纹理
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    // 设置纹理参数 - 优化GPU性能
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 预分配纹理存储（如果支持）
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glBindTexture(GL_TEXTURE_2D, 0);

    // 初始化PBO
    initializePBOs();

    checkGLError("initializeGL");
}

void VideoWidget::initializePBOs()
{
    // 生成PBO
    glGenBuffers(PBO_COUNT, pboIds);

    // 不预分配，等待第一帧时再分配
    checkGLError("initializePBOs");
}

void VideoWidget::initializeShaders()
{
    shaderProgram = new QOpenGLShaderProgram(this);

    // 添加顶点着色器
    if (!shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource)) {
        qDebug() << "Failed to compile vertex shader:" << shaderProgram->log();
    }

    // 添加片段着色器
    if (!shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource)) {
        qDebug() << "Failed to compile fragment shader:" << shaderProgram->log();
    }

    // 链接着色器程序
    if (!shaderProgram->link()) {
        qDebug() << "Failed to link shader program:" << shaderProgram->log();
    }

    // 缓存uniform位置
    shaderProgram->bind();
    uniformHasVideo = shaderProgram->uniformLocation("hasVideo");
    uniformVideoTexture = shaderProgram->uniformLocation("videoTexture");
    uniformIsYUV = shaderProgram->uniformLocation("isYUV");
    shaderProgram->release();
}

void VideoWidget::initializeVertexData()
{
    // 使用标准化设备坐标，优化GPU顶点处理
    float vertices[] = {
        // 位置        纹理坐标
        -1.0f,  1.0f,  0.0f, 0.0f,  // 左上
        -1.0f, -1.0f,  0.0f, 1.0f,  // 左下
        1.0f, -1.0f,  1.0f, 1.0f,  // 右下

        -1.0f,  1.0f,  0.0f, 0.0f,  // 左上
        1.0f, -1.0f,  1.0f, 1.0f,  // 右下
        1.0f,  1.0f,  1.0f, 0.0f   // 右上
    };

    // 创建VAO
    vao.create();
    vao.bind();

    // 创建并绑定顶点缓冲
    vertexBuffer.create();
    vertexBuffer.bind();
    vertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    vertexBuffer.allocate(vertices, sizeof(vertices));

    // 设置顶点属性
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    vao.release();
    vertexBuffer.release();

    checkGLError("initializeVertexData");
}

void VideoWidget::resizeGL(int w, int h)
{
    // 直接设置视口，不做额外的Qt操作
    glViewport(0, 0, w, h);
}

void VideoWidget::paintGL()
{
    // 清除颜色缓冲
    glClear(GL_COLOR_BUFFER_BIT);

    // 检查是否有准备好的帧
    int frameToRender = readyFrameIndex.load();
    if (frameToRender >= 0) {
        std::lock_guard<std::mutex> lock(frameMutex);
        FrameBuffer& frame = frameBuffers[frameToRender];

        if (frame.ready && frame.data && frame.width > 0 && frame.height > 0) {
            glBindTexture(GL_TEXTURE_2D, textureId);

            // 计算数据大小
            size_t dataSize = frame.width * frame.height * 3;

            // 检查是否需要重新分配纹理和PBO
            if (!textureCreated || videoWidth != frame.width || videoHeight != frame.height) {
                videoWidth = frame.width;
                videoHeight = frame.height;

                // 使用不可变纹理存储（如果支持）
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8,
                             videoWidth, videoHeight, 0,
                             GL_RGB, GL_UNSIGNED_BYTE, nullptr);

                // 重新分配所有PBO
                for (int i = 0; i < PBO_COUNT; i++) {
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds[i]);
                    glBufferData(GL_PIXEL_UNPACK_BUFFER, dataSize, nullptr, GL_STREAM_DRAW);
                }

                pboSize = dataSize;
                textureCreated = true;

                checkGLError("Texture/PBO allocation");
            }

            // 使用当前PBO上传纹理
            int currentPbo = currentPboIndex.load();
            int nextPbo = (currentPbo + 1) % PBO_COUNT;

            // 绑定当前PBO并从中更新纹理
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds[currentPbo]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            videoWidth, videoHeight,
                            GL_RGB, GL_UNSIGNED_BYTE, nullptr);

            // 绑定下一个PBO并写入新数据
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboIds[nextPbo]);

            // 使用持久映射如果可用，否则使用标准映射
            void* pboMemory = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, dataSize,
                                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (pboMemory) {
                memcpy(pboMemory, frame.data.get(), dataSize);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
            }

            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, 0);

            // 更新PBO索引
            currentPboIndex.store(nextPbo);

            hasVideo = true;
            frameCount++;

            // 标记帧已处理
            frame.ready = false;
            readyFrameIndex.store(-1);

            checkGLError("Texture upload");
        }
    }

    // 渲染
    if (hasVideo && textureCreated) {
        shaderProgram->bind();

        // 使用缓存的uniform位置
        glUniform1i(uniformHasVideo, 1);
        glUniform1i(uniformIsYUV, 0);

        // 激活纹理单元0并绑定纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glUniform1i(uniformVideoTexture, 0);

        // 绘制
        vao.bind();
        glDrawArrays(GL_TRIANGLES, 0, 6);
        vao.release();

        glBindTexture(GL_TEXTURE_2D, 0);
        shaderProgram->release();

        checkGLError("Rendering");
    }
}

void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame || !frame->data) {
        return;
    }

    // 使用三重缓冲
    int nextFrameIndex = (currentFrameIndex.load() + 1) % 3;

    {
        std::lock_guard<std::mutex> lock(frameMutex);
        FrameBuffer& buffer = frameBuffers[nextFrameIndex];

        buffer.data = frame->data;
        buffer.width = frame->width;
        buffer.height = frame->height;
        buffer.ready = true;
    }

    currentFrameIndex.store(nextFrameIndex);
    readyFrameIndex.store(nextFrameIndex);

    // 请求重绘 - 这会触发paintGL
    update();
}

void VideoWidget::clearDisplay()
{
    hasVideo = false;
    readyFrameIndex.store(-1);

    {
        std::lock_guard<std::mutex> lock(frameMutex);
        for (auto& buffer : frameBuffers) {
            buffer.ready = false;
            buffer.data.reset();
        }
    }

    update();
}

void VideoWidget::updateFPS()
{
    qint64 elapsed = fpsTimer.elapsed();
    if (elapsed > 0) {
        double fps = (frameCount.load() * 1000.0) / elapsed;
        currentFPS.store(fps);
        frameCount.store(0);
        fpsTimer.restart();
    }
}

void VideoWidget::initKeyMap()
{
    keyMap.clear();

    // 基本控制键
    keyMap.insert(Qt::Key_Left, VK_LEFT);
    keyMap.insert(Qt::Key_Up, VK_UP);
    keyMap.insert(Qt::Key_Right, VK_RIGHT);
    keyMap.insert(Qt::Key_Down, VK_DOWN);

    // 特殊键
    keyMap.insert(Qt::Key_Backspace, VK_BACK);
    keyMap.insert(Qt::Key_Tab, VK_TAB);
    keyMap.insert(Qt::Key_Return, VK_RETURN);
    keyMap.insert(Qt::Key_Enter, VK_RETURN);
    keyMap.insert(Qt::Key_Shift, VK_SHIFT);
    keyMap.insert(Qt::Key_Control, VK_CONTROL);
    keyMap.insert(Qt::Key_Alt, VK_MENU);
    keyMap.insert(Qt::Key_Escape, VK_ESCAPE);
    keyMap.insert(Qt::Key_Space, VK_SPACE);

    // 数字键 (0-9)
    for (int i = 0; i <= 9; i++) {
        keyMap.insert(Qt::Key_0 + i, 0x30 + i);
    }

    // 字母键 (A-Z)
    for (int i = 0; i < 26; i++) {
        keyMap.insert(Qt::Key_A + i, 0x41 + i);
    }

    // 功能键 (F1-F24)
    for (int i = 0; i < 24; i++) {
        keyMap.insert(Qt::Key_F1 + i, VK_F1 + i);
    }

    // ===== 重要：添加Shift+数字键的直接映射 =====
    keyMap.insert(Qt::Key_Exclam, 0x31);      // ! -> 1键
    keyMap.insert(Qt::Key_At, 0x32);          // @ -> 2键
    keyMap.insert(Qt::Key_NumberSign, 0x33);  // # -> 3键
    keyMap.insert(Qt::Key_Dollar, 0x34);      // $ -> 4键
    keyMap.insert(Qt::Key_Percent, 0x35);     // % -> 5键
    keyMap.insert(Qt::Key_AsciiCircum, 0x36); // ^ -> 6键
    keyMap.insert(Qt::Key_Ampersand, 0x37);   // & -> 7键
    keyMap.insert(Qt::Key_Asterisk, 0x38);    // * -> 8键
    keyMap.insert(Qt::Key_ParenLeft, 0x39);   // ( -> 9键
    keyMap.insert(Qt::Key_ParenRight, 0x30);  // ) -> 0键

    // ===== 其他Shift+符号键的映射 =====
    keyMap.insert(Qt::Key_Colon, VK_OEM_1);        // : -> ;键
    keyMap.insert(Qt::Key_Plus, VK_OEM_PLUS);      // + -> =键
    keyMap.insert(Qt::Key_Less, VK_OEM_COMMA);     // < -> ,键
    keyMap.insert(Qt::Key_Underscore, VK_OEM_MINUS);// _ -> -键
    keyMap.insert(Qt::Key_Greater, VK_OEM_PERIOD); // > -> .键
    keyMap.insert(Qt::Key_Question, VK_OEM_2);     // ? -> /键
    keyMap.insert(Qt::Key_BraceLeft, VK_OEM_4);    // { -> [键
    keyMap.insert(Qt::Key_Bar, VK_OEM_5);          // | -> \键
    keyMap.insert(Qt::Key_BraceRight, VK_OEM_6);   // } -> ]键
    keyMap.insert(Qt::Key_QuoteDbl, VK_OEM_7);     // " -> '键
    keyMap.insert(Qt::Key_AsciiTilde, VK_OEM_3);   // ~ -> `键

    // ===== 基础符号键（不需要Shift） =====
    keyMap.insert(Qt::Key_Semicolon, VK_OEM_1);      // ;
    keyMap.insert(Qt::Key_Equal, VK_OEM_PLUS);       // =
    keyMap.insert(Qt::Key_Comma, VK_OEM_COMMA);      // ,
    keyMap.insert(Qt::Key_Minus, VK_OEM_MINUS);      // -
    keyMap.insert(Qt::Key_Period, VK_OEM_PERIOD);    // .
    keyMap.insert(Qt::Key_Slash, VK_OEM_2);          // /
    keyMap.insert(Qt::Key_BracketLeft, VK_OEM_4);    // [
    keyMap.insert(Qt::Key_Backslash, VK_OEM_5);      // \
    keyMap.insert(Qt::Key_BracketRight, VK_OEM_6);   // ]
    keyMap.insert(Qt::Key_Apostrophe, VK_OEM_7);     // '
    keyMap.insert(Qt::Key_QuoteLeft, VK_OEM_3);      // `
}

// 事件处理函数保持不变...
void VideoWidget::wheelEvent(QWheelEvent *event)
{
    int wheelValue = 0;

    if (!event->pixelDelta().isNull()) {
        wheelValue = event->pixelDelta().y();
    }
    else if (!event->angleDelta().isNull()) {
        wheelValue = event->angleDelta().y();
    } else {
        return;
    }

    short wheelType = 5;
    size_t total = sizeof(short) + sizeof(int);
    unsigned char * wheelEventData = new unsigned char[total];

    std::memcpy(wheelEventData, &wheelType, sizeof(short));
    std::memcpy(wheelEventData + sizeof(short), &wheelValue, sizeof(int));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(wheelEventData, total);
    } else {
        delete[] wheelEventData;
    }
}

void VideoWidget::mousePressEvent(QMouseEvent* event)
{
    int widgetX = event->pos().x();
    int widgetY = event->pos().y();

    int videoX = 0;
    int videoY = 0;

    if (videoWidth > 0 && videoHeight > 0) {
        videoX = (widgetX * videoWidth) / width();
        videoY = (widgetY * videoHeight) / height();
        videoX = qBound(0, videoX, videoWidth - 1);
        videoY = qBound(0, videoY, videoHeight - 1);
    }

    short mouseType = 0;
    if (event->button() == Qt::LeftButton) {
        mouseType = 0;
    }
    else if (event->button() == Qt::RightButton) {
        mouseType = 1;
    }
    else if (event->button() == Qt::MiddleButton) {
        mouseType = 2;
    }

    size_t total = sizeof(int) * 2 + sizeof(short) * 2;
    unsigned char* mousePress = new unsigned char[total];
    short type = 1;

    std::memcpy(mousePress, &type, sizeof(short));
    std::memcpy(mousePress + sizeof(short), &mouseType, sizeof(short));
    std::memcpy(mousePress + sizeof(short) * 2, &videoX, sizeof(int));
    std::memcpy(mousePress + sizeof(short) * 2 + sizeof(int), &videoY, sizeof(int));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(mousePress, total);
    } else {
        delete[] mousePress;
    }
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* event)
{
    int widgetX = event->pos().x();
    int widgetY = event->pos().y();

    int videoX = 0;
    int videoY = 0;

    if (videoWidth > 0 && videoHeight > 0) {
        videoX = (widgetX * videoWidth) / width();
        videoY = (widgetY * videoHeight) / height();
        videoX = qBound(0, videoX, videoWidth - 1);
        videoY = qBound(0, videoY, videoHeight - 1);
    }

    short mouseType = 0;
    if (event->button() == Qt::LeftButton) {
        mouseType = 0;
    }
    else if (event->button() == Qt::RightButton) {
        mouseType = 1;
    }
    else if (event->button() == Qt::MiddleButton) {
        mouseType = 2;
    }

    size_t total = sizeof(int) * 2 + sizeof(short) * 2;
    unsigned char* mousePress = new unsigned char[total];
    short type = 2;

    std::memcpy(mousePress, &type, sizeof(short));
    std::memcpy(mousePress + sizeof(short), &mouseType, sizeof(short));
    std::memcpy(mousePress + sizeof(short) * 2, &videoX, sizeof(int));
    std::memcpy(mousePress + sizeof(short) * 2 + sizeof(int), &videoY, sizeof(int));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(mousePress, total);
    } else {
        delete[] mousePress;
    }
}

void VideoWidget::mouseMoveEvent(QMouseEvent* event)
{
    int widgetX = event->pos().x();
    int widgetY = event->pos().y();

    if (lastSentMousePos.x() >= 0) {
        int dx = abs(widgetX - lastSentMousePos.x());
        int dy = abs(widgetY - lastSentMousePos.y());

        if (dx < MOUSE_MOVE_THRESHOLD && dy < MOUSE_MOVE_THRESHOLD) {
            return;
        }
    }

    lastSentMousePos = event->pos();

    int videoX = 0;
    int videoY = 0;

    if (videoWidth > 0 && videoHeight > 0) {
        videoX = (widgetX * videoWidth) / width();
        videoY = (widgetY * videoHeight) / height();
        videoX = qBound(0, videoX, videoWidth - 1);
        videoY = qBound(0, videoY, videoHeight - 1);
    }

    size_t total = sizeof(int) * 2 + sizeof(short);
    unsigned char* mouseMovePress = new unsigned char[total];
    short type = 0;

    std::memcpy(mouseMovePress, &type, sizeof(short));
    std::memcpy(mouseMovePress + sizeof(short), &videoX, sizeof(int));
    std::memcpy(mouseMovePress + sizeof(short) + sizeof(int), &videoY, sizeof(int));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(mouseMovePress, total);
    } else {
        delete[] mouseMovePress;
    }
}

void VideoWidget::keyPressEvent(QKeyEvent* event)
{
    int qtKey = event->key();

    if (!keyMap.contains(qtKey)) {
        qDebug() << "Unmapped key:" << Qt::hex << qtKey << event->text();
        return;
    }

    unsigned char windowsKey = keyMap[qtKey];
    short type = 3;

    bool shiftPressed = event->modifiers() & Qt::ShiftModifier;
    bool ctrlPressed = event->modifiers() & Qt::ControlModifier;
    bool altPressed = event->modifiers() & Qt::AltModifier;

    size_t total = sizeof(short) + sizeof(char) + sizeof(char);
    unsigned char* keyPress = new unsigned char[total];

    char modifiers = 0;
    if (shiftPressed) modifiers |= 0x01;
    if (ctrlPressed) modifiers |= 0x02;
    if (altPressed) modifiers |= 0x04;

    std::memcpy(keyPress, &type, sizeof(short));
    std::memcpy(keyPress + sizeof(short), &windowsKey, sizeof(char));
    std::memcpy(keyPress + sizeof(short) + sizeof(char), &modifiers, sizeof(char));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(keyPress, total);
    } else {
        delete[] keyPress;
    }
}

void VideoWidget::keyReleaseEvent(QKeyEvent* event)
{
    int qtKey = event->key();

    if (!keyMap.contains(qtKey)) {
        qDebug() << "Unmapped key:" << Qt::hex << qtKey << event->text();
        return;
    }

    unsigned char windowsKey = keyMap[qtKey];
    short type = 4;

    bool shiftPressed = event->modifiers() & Qt::ShiftModifier;
    bool ctrlPressed = event->modifiers() & Qt::ControlModifier;
    bool altPressed = event->modifiers() & Qt::AltModifier;

    size_t total = sizeof(short) + sizeof(char) + sizeof(char);
    unsigned char* keyPress = new unsigned char[total];

    char modifiers = 0;
    if (shiftPressed) modifiers |= 0x01;
    if (ctrlPressed) modifiers |= 0x02;
    if (altPressed) modifiers |= 0x04;

    std::memcpy(keyPress, &type, sizeof(short));
    std::memcpy(keyPress + sizeof(short), &windowsKey, sizeof(char));
    std::memcpy(keyPress + sizeof(short) + sizeof(char), &modifiers, sizeof(char));

    if (webRTCRemoteClient) {
        webRTCRemoteClient->writerRemote(keyPress, total);
    } else {
        delete[] keyPress;
    }
}
