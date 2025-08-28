#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <memory>
#include <atomic>
#include <QMap>
#include <QElapsedTimer>
#include <mutex>

class WebRTCRemoteClient;
struct VideoFrame;

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

    // 显示视频帧
    void displayFrame(std::shared_ptr<VideoFrame> frame);

    // 清空显示
    void clearDisplay();

    // 获取当前帧率
    double getFrameRate() const { return currentFPS; }

    // 全屏控制
    void enterFullScreen();
    void exitFullScreen();
    bool isInFullScreenMode() const { return isFullScreenMode; }

    WebRTCRemoteClient* webRTCRemoteClient;

protected:
    // OpenGL函数
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // 事件处理
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void updateFPS();
    void onFullScreenClicked();
    void onExitFullScreenClicked();
    void checkMousePosition();
    void hideSidebar();

private:
    // 初始化着色器
    void initializeShaders();

    // 初始化顶点数据
    void initializeVertexData();

    // 初始化按键映射
    void initKeyMap();

    // 初始化UI控件
    void initializeControls();

    // 更新控件位置
    void updateControlsPosition();

    // 更新侧边栏位置
    void updateSidebarPosition();

    // 显示/隐藏侧边栏
    void showSidebar();

    // 检查OpenGL错误
    void checkGLError(const char* operation);

    // 初始化PBO
    void initializePBOs();

private:
    // OpenGL资源
    QOpenGLShaderProgram* shaderProgram;
    GLuint textureId;
    QOpenGLBuffer vertexBuffer;
    QOpenGLVertexArrayObject vao;

    // PBO相关 - 三重缓冲
    static constexpr int PBO_COUNT = 3;
    GLuint pboIds[PBO_COUNT];
    std::atomic<int> currentPboIndex;
    std::atomic<int> uploadPboIndex;
    size_t pboSize;

    // 视频数据缓冲
    struct FrameBuffer {
        std::shared_ptr<uint8_t[]> data;
        int width;
        int height;
        bool ready;
    };

    FrameBuffer frameBuffers[3];
    std::atomic<int> currentFrameIndex;
    std::atomic<int> readyFrameIndex;
    std::mutex frameMutex;

    // 视频信息
    int videoWidth;
    int videoHeight;
    bool textureCreated;

    // 帧率统计
    QElapsedTimer fpsTimer;
    std::atomic<int> frameCount;
    std::atomic<double> currentFPS;
    qint64 lastFPSUpdate;

    // 状态信息
    std::atomic<bool> hasVideo;
    std::atomic<bool> needsTextureUpdate;

    // 按键映射
    QMap<int, char> keyMap;

    // 着色器源码
    static const char* vertexShaderSource;
    static const char* fragmentShaderSource;

    // 鼠标移动优化
    QPoint lastSentMousePos{-1, -1};
    static constexpr int MOUSE_MOVE_THRESHOLD = 3;

    // GPU相关设置
    bool gpuAccelerationEnabled;
    int maxTextureSize;

    // Uniform位置缓存
    GLint uniformHasVideo;
    GLint uniformVideoTexture;
    GLint uniformIsYUV;

    // 全屏功能相关
    QPushButton* fullScreenButton;        // 进入全屏按钮
    QPushButton* exitFullScreenButton;    // 退出全屏按钮

    // 侧边栏相关
    QWidget* sidebar;                     // 侧边栏（普通模式和全屏模式共用）
    QPushButton* sidebarExitButton;       // 兼容性指针，指向exitFullScreenButton
    QTimer* mouseCheckTimer;              // 检查鼠标位置的定时器
    QTimer* hideTimer;                    // 自动隐藏侧边栏的定时器
    QPropertyAnimation* sidebarAnimation; // 侧边栏动画

    bool isFullScreenMode;                // 是否处于全屏模式
    QRect normalGeometry;                 // 保存普通窗口的几何信息
    Qt::WindowStates normalWindowState;   // 保存普通窗口状态
    bool sidebarVisible;                  // 侧边栏是否可见

    // 侧边栏常量
    static constexpr int SIDEBAR_WIDTH = 30;
    static constexpr int SIDEBAR_TRIGGER_ZONE = 1;  // 触发区域宽度
    static constexpr int HIDE_DELAY = 1500;          // 自动隐藏延迟（毫秒）

    // QWidget interface
protected:
    void enterEvent(QEnterEvent *event);
    void leaveEvent(QEvent *event);
};
