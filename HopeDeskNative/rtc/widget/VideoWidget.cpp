#include "VideoWidget.h"
#include "../WebrtcManager.h"
#include <rhi/qrhi_platform.h>
#include <QVBoxLayout>
#include <QFile>
#include <QCursor>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QDir>
#include <QWindow>
#include <QMetaObject>
#include <algorithm>
#include <thread>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>   // IDXGIKeyedMutex(渲染读前 acquire,等解码写完成)
#include <wrl/client.h>
#include "../../utils/Utils.h"

namespace hope {
namespace rtc {

// 帧队列上限:解码突发时丢最旧帧,保持渲染延迟最小。
static constexpr int kMaxQueuedFrames = 4;

VideoWidget::VideoWidget(QWidget* parent)
    : QRhiWidget(parent)
    , webrtcManager(nullptr)
    , rhi(nullptr)
    , videoWidth(640)
    , videoHeight(480)
    , resourcesInitialized(false)
    , interceptionHook(nullptr)
    , currentFrameFormat(FrameFormat::Unknown)
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
    fpsUpdateTimer = new QTimer(this);
    connect(fpsUpdateTimer, &QTimer::timeout, this, &VideoWidget::updateFPS);
    // 默认不统计帧率,由 MainWindow 通过 setFpsEnabled 按设置开启

    lastUniformData.mvp.setToIdentity();
    lastUniformData.params = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
    lastUniformData.uvScale = QVector2D(1.0f, 1.0f);

    LOG_INFO("VideoWidget init finished");
}

VideoWidget::~VideoWidget()
{
    LOG_INFO("VideoWidget destruction");
    std::shared_ptr<VideoFrame> dropped;
    while (frameQueue.try_dequeue(dropped)) {}   // 释放未渲染帧(shared_ptr 自动释放)
    lastRenderedFrame.reset();
    savePipelineCache();
}

void VideoWidget::initialize(QRhiCommandBuffer* cb)
{
    if (!QRhiWidget::rhi()) {
        LOG_ERROR("RHI not initialized");
        return;
    }

    injectD3D11DeviceToManager();

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
    // 裸 D3D11 NV12 渲染器(硬解帧用):需 QRhi 的 D3D11 设备/上下文。
    const QRhiD3D11NativeHandles* nativeHandles =
        static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    if (nativeHandles && nativeHandles->dev && nativeHandles->context) {
        nv12RawRenderer = std::make_unique<D3D11Nv12Renderer>();
        if (!nv12RawRenderer->init(reinterpret_cast<ID3D11Device*>(nativeHandles->dev),
                                   reinterpret_cast<ID3D11DeviceContext*>(nativeHandles->context))) {
            LOG_ERROR("D3D11Nv12Renderer init failed, Nv12Gpu frames will be dropped");
            nv12RawRenderer.reset();
        }
    }

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

    // NV12 交错 UV 平面(RG8),与 I420 的 U/V 同尺寸
    videoTextureUV.reset(rhi->newTexture(QRhiTexture::RG8, QSize(chromaWidth, chromaHeight), 1));
    if (!videoTextureUV || !videoTextureUV->create()) {
        LOG_ERROR("NV12: videoTextureUV(RG8 %dx%d) create failed", chromaWidth, chromaHeight);
        videoTextureUV.reset();
    }

    // I420 拆三平面:Y 用上面的 videoTextureY,这里建 U、V 两个 R8 色度纹理。
    videoTextureU.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    if (!videoTextureU || !videoTextureU->create()) {
        LOG_ERROR("I420: videoTextureU(R8 %dx%d) create failed", chromaWidth, chromaHeight);
        videoTextureU.reset();
    }
    videoTextureV.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    if (!videoTextureV || !videoTextureV->create()) {
        LOG_ERROR("I420: videoTextureV(R8 %dx%d) create failed", chromaWidth, chromaHeight);
        videoTextureV.reset();
    }

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
    if (!rhi || !uniformBuffer || !sampler) return;

    // I420 管线:Y/U/V 三平面独立纹理(binding 1/2/3),全幅采样。
    if (videoTextureY && videoTextureU && videoTextureV) {
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

    // NV12 管线:Y(R8, binding1) + UV 交错(RG8, binding2)
    if (videoTextureY && videoTextureUV) {
        nv12Srb.reset(rhi->newShaderResourceBindings());
        nv12Srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                uniformBuffer.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                videoTextureY.get(), sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                videoTextureUV.get(), sampler.get())
        });
        if (!nv12Srb->create()) {
            LOG_ERROR("NV12: nv12Srb create failed");
            nv12Srb.reset();
        }
    }
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

    if (nv12Srb) {
        nv12Pipeline.reset(rhi->newGraphicsPipeline());
        QShader nv12Frag = getShader(":/shaders/res/video_nv12.frag.qsb");
        if (nv12Frag.isValid()) {
            nv12Pipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vertShader },
                { QRhiShaderStage::Fragment, nv12Frag }
            });
            nv12Pipeline->setVertexInputLayout(inputLayout);
            nv12Pipeline->setShaderResourceBindings(nv12Srb.get());
            nv12Pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            nv12Pipeline->setDepthTest(false);
            nv12Pipeline->setDepthWrite(false);
            nv12Pipeline->setCullMode(QRhiGraphicsPipeline::None);
            if (!nv12Pipeline->create()) {
                LOG_ERROR("NV12: nv12Pipeline create failed (video_nv12.frag.qsb 与当前 RHI 后端不兼容?)");
                nv12Pipeline.reset();
            }
        } else {
            LOG_ERROR("Invalid video_nv12.frag shader");
            nv12Pipeline.reset();
        }
    }
}

void VideoWidget::clearDisplay()
{
    hasVideo = false;
    std::shared_ptr<VideoFrame> dropped;
    while (frameQueue.try_dequeue(dropped)) {}   // shared_ptr 自动释放
    lastRenderedFrame.reset();         // 清掉重绘源,避免清屏后 redraw 残留旧帧
    update();
}

void VideoWidget::displayFrame(std::shared_ptr<VideoFrame> frame)
{
    if (!frame) return;
    // Nv12Gpu 帧数据在 d3d11FrameData(buffer/nv12Buffer 为空),不能只认这两个。
    if (!frame->buffer && !frame->nv12Buffer && !frame->d3d11FrameData) return;

    // RAII 队列入队;解码快于渲染时丢最旧帧控延迟(丢的帧析构自动回槽位)。
    frameQueue.enqueue(std::move(frame));
    std::shared_ptr<VideoFrame> dropped;
    while (frameQueue.size_approx() > kMaxQueuedFrames && frameQueue.try_dequeue(dropped)) {
    }

    this->update();
}

void VideoWidget::ensureTexturesForSize(int width, int height)
{
    if (width == texWidth && height == texHeight && videoTextureY && videoTextureUV && videoTextureU && videoTextureV)
        return; // 尺寸未变，纹理有效

    // 销毁旧纹理并重建，同时重建 SRB/pipeline（因为纹理绑定变了）
    videoTextureY.reset();
    videoTextureUV.reset();
    videoTextureU.reset();
    videoTextureV.reset();

    videoTextureY.reset(rhi->newTexture(QRhiTexture::R8, QSize(width, height), 1));
    videoTextureY->create();

    int chromaWidth = (width + 1) / 2;
    int chromaHeight = (height + 1) / 2;
    videoTextureUV.reset(rhi->newTexture(QRhiTexture::RG8, QSize(chromaWidth, chromaHeight), 1));
    videoTextureUV->create();

    // I420 色度平面(与 createTextures 保持一致)
    videoTextureU.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    videoTextureU->create();
    videoTextureV.reset(rhi->newTexture(QRhiTexture::R8, QSize(chromaWidth, chromaHeight), 1));
    videoTextureV->create();

    texWidth = width;
    texHeight = height;
    createShaderResourceBindings(); // SRB 需要重新绑定新纹理
    createPipeline();               // pipeline 依赖于 SRB
}

void VideoWidget::render(QRhiCommandBuffer* cb) {
    // 基础检查
    if (!rhi || !resourcesInitialized || !renderTarget()) {
        if (renderTarget()) {
            const QColor clearColor(32, 32, 32);
            cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 });
            cb->endPass();
        }
        return;
    }

    // 1. 取队列最新帧:排空积压的旧帧、只取最新一帧绘制。渲染跟不上解码时,
    //    FIFO 逐帧取会造成"显示旧帧 -> 突然跳最新"的画面跳动;取最新帧则始终
    //    显示当前最新画面,节奏平滑。被丢的旧帧未绘制,槽位安全回池。
    std::shared_ptr<VideoFrame> popped;
    while (frameQueue.try_dequeue(popped)) {}
    const bool hasNewFrame = (popped != nullptr);
    std::shared_ptr<VideoFrame> frameToRender = hasNewFrame ? popped : nullptr;

    // 不再默认使用 pipeline (I420)，初始化为 nullptr，防止无帧时用错管线
    QRhiGraphicsPipeline* activePipeline = nullptr;
    QRhiShaderResourceBindings* activeSrb = nullptr;

    // 2. 如果有新帧，进行纹理上传/导入和管线选择
    // 2a. 零拷贝 GPU 路径:D3D11 解码产出的 NV12 共享纹理 + plane SRV,裸 D3D11 直接画,不碰 CPU
    bool nv12GpuReady = false;
    int gpuW = 0, gpuH = 0;
    if (frameToRender && frameToRender->format == FrameFormat::Nv12Gpu &&
        frameToRender->d3d11FrameData && frameToRender->d3d11FrameData->nv12Texture &&
        frameToRender->d3d11FrameData->planeYSrv && frameToRender->d3d11FrameData->planeUvSrv &&
        nv12RawRenderer) {
        nv12GpuReady = true;
        gpuW = frameToRender->width;
        gpuH = frameToRender->height;
    }
    // 2b. CPU 路径(上传):NV12 / I420
    else if (frameToRender && (frameToRender->buffer || frameToRender->nv12Buffer)) {
        int srcWidth = frameToRender->width;
        int srcHeight = frameToRender->height;

        // 动态调整纹理尺寸
        ensureTexturesForSize(srcWidth, srcHeight);

        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
        bool uploaded = false;

        // --- NV12 硬解回退路径(D3D11 解码 -> NV12Buffer -> 上传) ---
        if (frameToRender->format == FrameFormat::Nv12 && frameToRender->nv12Buffer &&
            videoTextureY && videoTextureUV && nv12Pipeline && nv12Srb) {

            auto* nv12 = frameToRender->nv12Buffer.get();
            int chromaW = (srcWidth + 1) / 2;
            int chromaH = (srcHeight + 1) / 2;

            // 上传 Y 平面
            QRhiTextureSubresourceUploadDescription subY(nv12->DataY(), nv12->StrideY() * srcHeight);
            subY.setSourceSize(QSize(srcWidth, srcHeight));
            subY.setDataStride(nv12->StrideY());
            batch->uploadTexture(videoTextureY.get(), QRhiTextureUploadDescription{{0, 0, subY}});

            // 上传 UV 交错平面
            QRhiTextureSubresourceUploadDescription subUV(nv12->DataUV(), nv12->StrideUV() * chromaH);
            subUV.setSourceSize(QSize(chromaW, chromaH));
            subUV.setDataStride(nv12->StrideUV());
            batch->uploadTexture(videoTextureUV.get(), QRhiTextureUploadDescription{{0, 0, subUV}});

            activePipeline = nv12Pipeline.get();
            activeSrb = nv12Srb.get();
            currentFrameFormat = FrameFormat::Nv12; // 记录格式
            uploaded = true;

        }
        // --- I420 软解路径(拆 Y/U/V 三平面独立纹理,各一次全幅上传) ---
        else if (frameToRender->buffer && videoTextureY && videoTextureU && videoTextureV && pipeline && srb) {
            auto* i420 = frameToRender->buffer.get();
            int chromaW = (srcWidth + 1) / 2;
            int chromaH = (srcHeight + 1) / 2;

            // 三个平面分别上传到各自 R8 纹理,无坐标换算,shader 全幅采样。
            QRhiTextureSubresourceUploadDescription subY(i420->DataY(), i420->StrideY() * srcHeight);
            subY.setSourceSize(QSize(srcWidth, srcHeight));
            subY.setDataStride(i420->StrideY());
            batch->uploadTexture(videoTextureY.get(), QRhiTextureUploadDescription{{0, 0, subY}});

            QRhiTextureSubresourceUploadDescription subU(i420->DataU(), i420->StrideU() * chromaH);
            subU.setSourceSize(QSize(chromaW, chromaH));
            subU.setDataStride(i420->StrideU());
            batch->uploadTexture(videoTextureU.get(), QRhiTextureUploadDescription{{0, 0, subU}});

            QRhiTextureSubresourceUploadDescription subV(i420->DataV(), i420->StrideV() * chromaH);
            subV.setSourceSize(QSize(chromaW, chromaH));
            subV.setDataStride(i420->StrideV());
            batch->uploadTexture(videoTextureV.get(), QRhiTextureUploadDescription{{0, 0, subV}});

            activePipeline = pipeline.get();
            activeSrb = srb.get();
            currentFrameFormat = FrameFormat::I420; // 记录格式
            uploaded = true;
        }

        // 如果上传成功，更新 Uniform 并提交资源
        if (uploaded) {
            UniformData uniformData;
            uniformData.mvp.setToIdentity();
            uniformData.uvScale = QVector2D(1.0f, 1.0f);
            uniformData.params = QVector4D(1.0f, 0.0f, 1.0f, 0.0f); // I420/NV12 shader 均全幅采样,不用打包坐标

            if (uniformData != lastUniformData) {
                batch->updateDynamicBuffer(uniformBuffer.get(), 0, sizeof(UniformData), &uniformData);
                lastUniformData = uniformData;
            }

            cb->resourceUpdate(batch);
            hasVideo = true;
            videoWidth = srcWidth;
            videoHeight = srcHeight;
            if (fpsEnabled) frameCount++;
        } else {
            // 上传失败（格式不匹配等），清理帧,shared_ptr 自动释放
            frameToRender.reset();
        }
    }
    // 3. 如果没有新帧，但之前有视频，保持使用上一次的管线状态
    // 解决切换窗口回来瞬间 "activePipeline为空或错误" 导致的绿屏
    else if (hasVideo) {
        if (currentFrameFormat == FrameFormat::Nv12) {
            activePipeline = nv12Pipeline.get();
            activeSrb = nv12Srb.get();
        } else if (currentFrameFormat == FrameFormat::I420) {
            activePipeline = pipeline.get();
            activeSrb = srb.get();
        }
        // 此时不需要提交资源更新，只是复用上一帧的纹理状态进行绘制
    }

    // 4. 开始绘制
    const QColor clearColor = hasVideo ? Qt::black : QColor(48, 48, 48);
    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, nullptr);

    // 裸 D3D11 画 NV12 帧(新帧或定时器无新帧时重绘上一帧)。
    auto drawNv12 = [&](const std::shared_ptr<D3D11VideoFrameData>& fd) -> bool {
        if (!fd || !fd->planeYSrv || !fd->planeUvSrv || !nv12RawRenderer) return false;
        const QRhiD3D11NativeHandles* nh =
            static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        ID3D11DeviceContext* ctx = nh ? reinterpret_cast<ID3D11DeviceContext*>(nh->context) : nullptr;
        if (!ctx) return false;
        cb->beginExternal();
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            const QSize outSize = renderTarget()->pixelSize();
            nv12RawRenderer->draw(ctx, rtv, fd->planeYSrv.Get(), fd->planeUvSrv.Get(),
                                  outSize.width(), outSize.height());
            rtv->Release();
        }
        cb->endExternal();
        return true;
    };

    if (nv12GpuReady && frameToRender && frameToRender->d3d11FrameData) {
        if (drawNv12(frameToRender->d3d11FrameData)) {
            // 无需单独缓存:本帧已由本趟开头的 lastRenderedFrame 持有,无新帧时 redraw 直接画它
            currentFrameFormat = FrameFormat::Nv12Gpu;
            hasVideo = true;
            videoWidth = gpuW;
            videoHeight = gpuH;
            if (fpsEnabled) frameCount++;
        }
    }
    // 无新帧的 render(如 resize/expose 触发):重绘上一帧(不能走 CPU 路径,硬解流 CPU 纹理没数据会画黑)
    else if (hasVideo && currentFrameFormat == FrameFormat::Nv12Gpu && lastRenderedFrame &&
             lastRenderedFrame->d3d11FrameData) {
        drawNv12(lastRenderedFrame->d3d11FrameData);
    }
    // 4b. CPU 路径(上传后绘制):只有当管线和资源绑定有效时才绘制
    else if (hasVideo && activePipeline && activeSrb) {
        const QSize outputSize = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(activePipeline);
        cb->setViewport(QRhiViewport{0.0f, 0.0f, float(outputSize.width()), float(outputSize.height()), 0.0f, 1.0f});
        cb->setShaderResources(activeSrb);

        const QRhiCommandBuffer::VertexInput vbufBinding(vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(6);
    }

    cb->endPass();

    // 5. 延迟释放:新帧换入 lastRenderedFrame,上一帧(上一趟画的)此刻析构回池。
    if (hasNewFrame) {
        lastRenderedFrame = std::move(popped);
    }

    // 6. 持续渲染:在 render() 内调 update() 会形成"以 vsync 节流的持续渲染"
    //    (Qt QRhiWidget 文档)。每次显示刷新都呈现最新帧,节奏稳定平滑。
    //    开启 vsync(swap interval 1)时呈现对齐刷新率,无撕裂、无画面跳动。
    if (hasVideo) {
        this->update();
    }
}


void VideoWidget::releaseResources()
{
    nv12RawRenderer.reset();
    pipeline.reset();
    srb.reset();
    nv12Pipeline.reset();
    nv12Srb.reset();
    uniformBuffer.reset();
    videoTextureY.reset();
    videoTextureUV.reset();
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

void VideoWidget::setFpsEnabled(bool enabled)
{
    fpsEnabled = enabled;
    if (enabled) {
        frameCount = 0;
        currentFPS = 0.0;
        fpsTimer.restart();
        fpsUpdateTimer->start(1000);
    } else {
        if (fpsUpdateTimer) fpsUpdateTimer->stop();
        frameCount = 0;
        currentFPS = 0.0;
    }
}

void VideoWidget::setWebrtcManager(std::shared_ptr<WebrtcManager> webrtcManager)
{
    this->webrtcManager = webrtcManager;
    interceptionHook = std::make_unique<InterceptionHook>();
    interceptionHook->setTargetWidget(this);
    interceptionHook->setWebrtcManager(webrtcManager);
    interceptionHook->setVideoSize(width(), height());
    interceptionHook->startCapture();
}

void VideoWidget::injectD3D11DeviceToManager()
{
    QRhi* currentRhi = QRhiWidget::rhi();
    if (!currentRhi || !webrtcManager) return;
    const QRhiD3D11NativeHandles* nativeHandles =
        static_cast<const QRhiD3D11NativeHandles*>(currentRhi->nativeHandles());
    if (nativeHandles && nativeHandles->dev) {
        webrtcManager->setDecoderD3D11Device(
            reinterpret_cast<ID3D11Device*>(nativeHandles->dev));
    }
}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QRhiWidget::resizeEvent(event);
    if (interceptionHook) {
        interceptionHook->setVideoSize(event->size().width(), event->size().height());
    }
}

void VideoWidget::enterFullScreen()
{
    if (isFullScreenMode) return;

    normalGeometry = geometry();
    normalWindowState = windowState();

    isFullScreenMode = true;
    showFullScreen();   // 覆盖整个屏幕(含任务栏)

    LOG_INFO("Entering full screen mode");
}

void VideoWidget::exitFullScreen()
{
    if (!isFullScreenMode) return;

    isFullScreenMode = false;
    // 按进入全屏前的状态恢复:最大化 -> showMaximized;普通窗口 -> showNormal + 还原几何
    if (normalWindowState & Qt::WindowMaximized) {
        showMaximized();
    } else {
        showNormal();
        setGeometry(normalGeometry);
    }
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


void VideoWidget::enterEvent(QEnterEvent* event)
{
    QRhiWidget::enterEvent(event);
}


} // namespace rtc
} // namespace hope