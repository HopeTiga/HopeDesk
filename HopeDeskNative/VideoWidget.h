#pragma once
#include <QRhiWidget>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <QMouseEvent>
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
    void mouseMoveEvent(QMouseEvent* event) override;
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
    void createTextures(int width, int height);     // 现在接受尺寸
    void createSampler();
    void createShaderResourceBindings();
    void createPipeline();
    QShader getShader(const QString& name);
    void loadPipelineCache();
    void savePipelineCache();

    // 当视频尺寸变化时重建纹理和绑定
    void ensureTexturesForSize(int width, int height);

    WebRTCManager* manager;
    QRhi* rhi = nullptr;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;

    std::unique_ptr<QRhiBuffer> uniformBuffer;
    std::unique_ptr<QRhiTexture> videoTextureY;
    std::unique_ptr<QRhiTexture> videoTextureU;
    std::unique_ptr<QRhiTexture> videoTextureV;
    std::unique_ptr<QRhiShaderResourceBindings> srb;

    // 当前纹理的实际尺寸（与最新视频帧匹配）
    int texWidth = 0;
    int texHeight = 0;

    std::atomic<VideoFrame*> currentFramePtr{nullptr};
    std::atomic<VideoFrame*> frameToReleasePtr{nullptr};

    std::atomic<int> videoWidth{640};
    std::atomic<int> videoHeight{480};
    bool resourcesInitialized = false;

    QElapsedTimer fpsTimer;
    std::atomic<int> frameCount{0};
    std::atomic<double> currentFPS{0.0};
    std::atomic<bool> hasVideo{false};

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

    struct UniformData {
        QMatrix4x4 mvp;
        QVector4D params;
        QVector2D uvScale;   // 现在始终为 (1,1)
        QVector2D padding;

        bool operator!=(const UniformData& other) const {
            return mvp != other.mvp || params != other.params || uvScale != other.uvScale;
        }
    };
    UniformData lastUniformData;
};

} // namespace rtc
} // namespace hope
