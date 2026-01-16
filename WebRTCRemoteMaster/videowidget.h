#pragma once
#include <QRhiWidget>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <memory>
#include <atomic> // 必须包含
#include <mutex>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <array>
#include <chrono>

#include <api/video/video_frame.h>
#include <api/video/i420_buffer.h>

#include "windows.h"
#include "windowshook.h"
#include "Utils.h"
#include "interceptionhook.h"

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

    void loadPipelineCache();
    void savePipelineCache();

private:
    WebRTCManager* manager;
    QRhi* rhi = nullptr;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;
    // 主 SRB 仅用于 Pipeline 布局模板
    std::unique_ptr<QRhiShaderResourceBindings> srb;

    // 三缓冲 (Triple Buffering)
    static constexpr int FRAME_BUFFER_COUNT = 3;
    std::array<std::unique_ptr<QRhiBuffer>, FRAME_BUFFER_COUNT> uniformBuffers;
    std::array<std::unique_ptr<QRhiTexture>, FRAME_BUFFER_COUNT> videoTexturesY;
    std::array<std::unique_ptr<QRhiTexture>, FRAME_BUFFER_COUNT> videoTexturesU;
    std::array<std::unique_ptr<QRhiTexture>, FRAME_BUFFER_COUNT> videoTexturesV;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, FRAME_BUFFER_COUNT> perFrameSrb;

    // --- 核心优化：无锁环形队列结构 ---
    struct FrameBuffer {
        webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer;
        int width = 0;
        int height = 0;
        // 无需 ready/needsUpdate 标志，通过索引状态判断
    };

    std::array<FrameBuffer, FRAME_BUFFER_COUNT> frameBuffers;

    // 无锁队列索引
    // producerIdx: 生产者最新写入的帧索引 (Ready to read)
    // consumerIdx: 消费者正在显示的帧索引 (Displaying)
    std::atomic<int> producerIdx{-1};
    std::atomic<int> consumerIdx{-1};

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

    // --- 修复花屏的关键 1：强制内存对齐 ---
    struct UniformData {
        QMatrix4x4 mvp;     // 0-64
        QVector4D params;   // 64-80
        QVector2D uvScale;  // 80-88
        QVector2D padding;  // 88-96 (补齐 16 字节对齐)

        bool operator!=(const UniformData& other) const {
            return mvp != other.mvp || params != other.params || uvScale != other.uvScale;
        }
    };

    std::array<UniformData, FRAME_BUFFER_COUNT> lastUniformData;

    // 固定大纹理 (1080P)，如需 4K 请改为 3840x2160
    const QSize MAX_TEXTURE_SIZE = QSize(1920, 1080);

    std::chrono::steady_clock::time_point lastUpdateTime;
    static constexpr int MIN_FRAME_INTERVAL_MS = 1; // 解除锁帧，由 VSync 控制
};

}
}
