#pragma once
#include <QRhiWidget>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <QMouseEvent> // 用于事件驱动的鼠标追踪
#include <memory>
#include <atomic>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <chrono>

#include <api/video/video_frame.h>
#include <api/video/i420_buffer.h>

#include "windows.h"
#include "Utils.h"
#include "InterceptionHook.h"

namespace hope {
namespace rtc {

class WebRTCManager;
struct VideoFrame;

class VideoWidget : public QRhiWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

    // 接收视频帧（网络线程调用）
    void displayFrame(std::shared_ptr<VideoFrame> frame);

    void clearDisplay();
    double getFrameRate() const { return currentFPS; }

    void enterFullScreen();
    void exitFullScreen();
    bool isInFullScreenMode() const { return isFullScreenMode; }

    void setWebRTCManager(WebRTCManager* manager);

Q_SIGNALS:
    void disConnectRemote();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override; // 重写鼠标移动事件
    void closeEvent(QCloseEvent *event) override;

private Q_SLOTS:
    void updateFPS();
    void onFullScreenClicked();
    void onExitFullScreenClicked();
    void hideSidebar();

private:
    void initializeControls();
    void updateControlsPosition();
    void showSidebar();
    bool initializeResources(QRhiCommandBuffer* cb);
    void createBuffers();
    void createTextures();
    void createSampler();
    void createShaderResourceBindings();
    void createPipeline();
    QShader getShader(const QString& name);

    void loadPipelineCache();
    void savePipelineCache();

private:
    WebRTCManager* manager;
    QRhi* rhi = nullptr;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;

    // 单套资源（去除数组）
    std::unique_ptr<QRhiBuffer> uniformBuffer;
    std::unique_ptr<QRhiTexture> videoTextureY;
    std::unique_ptr<QRhiTexture> videoTextureU;
    std::unique_ptr<QRhiTexture> videoTextureV;
    std::unique_ptr<QRhiShaderResourceBindings> srb;

    // --- 极低延迟核心：无锁化双指针 ---
    std::atomic<VideoFrame*> currentFramePtr{nullptr};   // 当前待渲染的最新帧
    std::atomic<VideoFrame*> frameToReleasePtr{nullptr}; // 渲染完毕后丢弃的"垃圾桶"帧

    // 视频信息
    std::atomic<int> videoWidth{640};
    std::atomic<int> videoHeight{480};
    bool resourcesInitialized = false;

    QElapsedTimer fpsTimer;
    std::atomic<int> frameCount{0};
    std::atomic<double> currentFPS{0.0};
    std::atomic<bool> hasVideo{false};

    // UI控件
    QPushButton* fullScreenButton;
    QWidget* sidebar;
    QPushButton* sidebarExitButton;
    QTimer* hideTimer;
    QPropertyAnimation* sidebarAnimation;

    bool isFullScreenMode;
    QRect normalGeometry;
    Qt::WindowStates normalWindowState;
    bool sidebarVisible;

    static constexpr int SIDEBAR_WIDTH = 30;
    static constexpr int SIDEBAR_TRIGGER_ZONE = 1;
    static constexpr int HIDE_DELAY = 1500;

    std::unique_ptr<InterceptionHook> interceptionHook;

    // 内存对齐结构
    struct UniformData {
        QMatrix4x4 mvp;     // 0-64
        QVector4D params;   // 64-80
        QVector2D uvScale;  // 80-88
        QVector2D padding;  // 88-96 (补齐 16 字节对齐)

        bool operator!=(const UniformData& other) const {
            return mvp != other.mvp || params != other.params || uvScale != other.uvScale;
        }
    };

    UniformData lastUniformData;

    // 固定大纹理 (1080P)
    const QSize MAX_TEXTURE_SIZE = QSize(1920, 1080);
};

}
}
