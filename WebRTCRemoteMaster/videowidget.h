#pragma once
#include <QRhiWidget>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <memory>
#include <atomic>
#include <mutex>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <array>
#include <chrono>

#include "windows.h"

#include "windowshook.h"
#include "Utils.h"
#include "interceptionhook.h"

namespace hope{

namespace rtc{

class WebRTCManager;
struct VideoFrame;

class VideoWidget : public QRhiWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

    void displayFrame(std::shared_ptr<VideoFrame> frame);
    void clearDisplay();
    double getFrameRate() const { return currentFPS; }

    void enterFullScreen();
    void exitFullScreen();
    bool isInFullScreenMode() const { return isFullScreenMode; }

    void setWebRTCManager(WebRTCManager* manager);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private Q_SLOTS:
    void updateFPS();
    void onFullScreenClicked();
    void onExitFullScreenClicked();
    void checkMousePosition();
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

    // 新增：性能优化相关
    bool needsTextureResize(int width, int height, int slot);
    void resizeTextureIfNeeded(int slot, const QSize& newSize);
    void loadPipelineCache();
    void savePipelineCache();

private:
    WebRTCManager* manager;
    QRhi* rhi = nullptr;

    // 共享资源（所有帧共用）
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiShaderResourceBindings> srb;

    // Triple buffering资源（每帧独立）
    static constexpr int FRAME_BUFFER_COUNT = 3;
    std::array<std::unique_ptr<QRhiBuffer>, FRAME_BUFFER_COUNT> uniformBuffers;
    std::array<std::unique_ptr<QRhiTexture>, FRAME_BUFFER_COUNT> videoTextures;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, FRAME_BUFFER_COUNT> perFrameSrb;

    // 帧数据缓冲
    struct FrameBuffer {
        std::shared_ptr<uint8_t[]> data;
        int width = 0;
        int height = 0;
        bool ready = false;
        bool needsUpdate = false;
    };

    std::array<FrameBuffer, FRAME_BUFFER_COUNT> frameBuffers;
    std::atomic<int> currentFrameSlot{0};
    std::atomic<int> renderFrameIndex{0};
    std::mutex frameMutex;

    // 视频信息
    std::atomic<int> videoWidth{640};
    std::atomic<int> videoHeight{480};
    bool resourcesInitialized = false;

    // 帧率统计
    QElapsedTimer fpsTimer;
    std::atomic<int> frameCount{0};
    std::atomic<double> currentFPS{0.0};

    // 状态信息
    std::atomic<bool> hasVideo{false};

    // UI控件
    QPushButton* fullScreenButton;
    QWidget* sidebar;
    QPushButton* sidebarExitButton;
    QTimer* mouseCheckTimer;
    QTimer* hideTimer;
    QPropertyAnimation* sidebarAnimation;

    bool isFullScreenMode;
    QRect normalGeometry;
    Qt::WindowStates normalWindowState;
    bool sidebarVisible;

    static constexpr int SIDEBAR_WIDTH = 30;
    static constexpr int SIDEBAR_TRIGGER_ZONE = 1;
    static constexpr int HIDE_DELAY = 1500;

    std::unique_ptr<WindowsHook> windowsHook;
    std::unique_ptr<InterceptionHook> interceptionHook;

    // Uniform数据结构
    struct UniformData {
        QMatrix4x4 mvp;     // 64字节
        QVector4D params;   // 16字节 (x=hasVideo, y=isYUV, z=brightness, w=padding)

        bool operator!=(const UniformData& other) const {
            return mvp != other.mvp || params != other.params;
        }
    };

    // 优化：缓存上一次的uniform数据，避免不必要的更新
    std::array<UniformData, FRAME_BUFFER_COUNT> lastUniformData;

    // 优化：纹理尺寸配置
    static constexpr int MAX_TEXTURE_WIDTH = 1920;
    static constexpr int MAX_TEXTURE_HEIGHT = 1080;
    static constexpr int MIN_TEXTURE_RESIZE_THRESHOLD = 64; // 尺寸变化超过此值才重建

    // 优化：帧率限制
    std::chrono::steady_clock::time_point lastUpdateTime;
    static constexpr int MIN_FRAME_INTERVAL_MS = 8; // 最高60fps


};


}

}
