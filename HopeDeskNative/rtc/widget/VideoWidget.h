#pragma once
#include <QRhiWidget>
#include <QTimer>
#include <QPushButton>
#include <QWidget>
#include <QPropertyAnimation>
#include <QMouseEvent>
#include <memory>
#include <atomic>
#include <vector>
#include <QElapsedTimer>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <chrono>
#include <api/video/video_frame.h>
#include <api/video/i420_buffer.h>
#include "windows.h"
#include "../../utils/Utils.h"
#include "../../system/InterceptionHook.h"
#include "../codec/D3D11Nv12Renderer.h"
#include "../../utils/concurrentqueue.h"   // moodycamel 有界帧队列(RAII 交接)

namespace hope {
namespace rtc {

using hope::system::InterceptionHook;

class WebrtcManager;

enum class FrameFormat;

struct VideoFrame;

struct D3D11VideoFrameData;

class VideoWidget : public QRhiWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

    void displayFrame(std::shared_ptr<VideoFrame> frame);
    void clearDisplay();
    double getFrameRate() const { return currentFPS; }
    // 是否计算并统计渲染帧率(关闭时停止计时、不计 frameCount)
    void setFpsEnabled(bool enabled);
    void enterFullScreen();
    void exitFullScreen();
    bool isInFullScreenMode() const { return isFullScreenMode; }
    void setWebrtcManager(std::shared_ptr<WebrtcManager> webrtcManager);
    void injectD3D11DeviceToManager();

Q_SIGNALS:
    void disConnectRemote();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void closeEvent(QCloseEvent *event) override;

private Q_SLOTS:
    void updateFPS();
    void onFullScreenClicked();
    void onExitFullScreenClicked();

private:
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

private:

    FrameFormat currentFrameFormat;

    std::shared_ptr<WebrtcManager> webrtcManager;
    QRhi* rhi = nullptr;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::unique_ptr<QRhiBuffer> vertexBuffer;
    std::unique_ptr<QRhiSampler> sampler;

    std::unique_ptr<QRhiBuffer> uniformBuffer;
    std::unique_ptr<QRhiTexture> videoTextureY;      // Y 平面(R8):NV12 与 I420 共用
    std::unique_ptr<QRhiTexture> videoTextureUV;     // NV12 交错 UV 平面(RG8)
    // I420 拆三平面独立纹理:U/V 各为 R8(chromaW × chromaH),每平面一次全幅上传
    std::unique_ptr<QRhiTexture> videoTextureU;
    std::unique_ptr<QRhiTexture> videoTextureV;
    std::unique_ptr<QRhiShaderResourceBindings> srb;

    // NV12 渲染管线(硬解路径;I420 用上面的 pipeline/srb,软解保持不变)
    std::unique_ptr<QRhiGraphicsPipeline> nv12Pipeline;
    std::unique_ptr<QRhiShaderResourceBindings> nv12Srb;

    // 当前纹理的实际尺寸（与最新视频帧匹配）
    int texWidth = 0;
    int texHeight = 0;

    // 帧队列(RAII):多生产者(硬解直投/软解 sink)std::move 入队,渲染线程取走;
    // 帧持 keepAlive -> 槽位,任一方析构自动回池。解码快于渲染时丢最旧帧控延迟。
    moodycamel::ConcurrentQueue<std::shared_ptr<VideoFrame>> frameQueue;
    // 上一帧(仅 GUI 线程):延迟一帧释放 + 无新帧时重绘源。
    std::shared_ptr<VideoFrame> lastRenderedFrame;

    std::atomic<int> videoWidth{640};
    std::atomic<int> videoHeight{480};
    bool resourcesInitialized = false;

    QElapsedTimer fpsTimer;
    QTimer* fpsUpdateTimer = nullptr;
    std::atomic<int> frameCount{0};
    std::atomic<double> currentFPS{0.0};
    std::atomic<bool> fpsEnabled{false};
    std::atomic<bool> hasVideo{false};

    bool isFullScreenMode = false;
    QRect normalGeometry;
    Qt::WindowStates normalWindowState = Qt::WindowNoState;

    std::unique_ptr<InterceptionHook> interceptionHook;

    // 裸 D3D11 NV12 渲染器(QRhi 无法绑 NV12 纹理,硬解帧用 beginExternal 注入裸 D3D11 画)。
    std::unique_ptr<D3D11Nv12Renderer> nv12RawRenderer;

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