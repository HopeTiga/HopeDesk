#pragma once

#include "api/video_codecs/video_decoder.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"

#include <d3d11_1.h>
#include <d3d11_3.h>
#include <dxgi1_2.h>
#include <dxva.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "chromiumMedia/Av1Decoder.h"
#include "../../utils/concurrentqueue.h"

namespace hope {
namespace rtc {

class WebrtcVideoDecoderFactory;
class D3D11VideoFrameData;
struct VideoFrame;

class D3D11Av1VideoDecoder : public webrtc::VideoDecoder {
public:
    D3D11Av1VideoDecoder();
    ~D3D11Av1VideoDecoder() override;

    D3D11Av1VideoDecoder(const D3D11Av1VideoDecoder&) = delete;
    D3D11Av1VideoDecoder& operator=(const D3D11Av1VideoDecoder&) = delete;

    bool Configure(const Settings& settings) override;
    int32_t Decode(const webrtc::EncodedImage& inputImage,
                   bool missingFrames,
                   int64_t renderTimeMs) override;
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override;
    int32_t Release() override;
    DecoderInfo GetDecoderInfo() const override;

    // 注入渲染端(QRhi)的 D3D11 设备。解码设备必须与它同适配器(NT 共享句柄
    // 跨设备要求同适配器)。可在解码器创建后任意时刻调用。
    void setD3D11Device(ID3D11Device* dev);

    // 释放前唤醒解码线程(工厂在 peerConnection->Close() 前调用),无锁,不销毁资源。
    void requestRelease();

    // 记录创建者工厂,析构时从工厂注销(防悬垂)。
    void setOwnerFactory(WebrtcVideoDecoderFactory* f) { ownerFactory = f; }

    // 硬解帧直投 widget(绕过 track-sink);工厂注入,outputFrame 调用。软解仍走 sink。
    void setOnDisplayHandle(std::function<void(std::shared_ptr<VideoFrame>)> onDisplayHandle) {
        std::lock_guard<std::mutex> lock(mutex);
        this->onDisplayHandle = std::move(onDisplayHandle);
    }

    struct Slot {
        // ZeroCopy 模式:单共享纹理直解码(BIND_DECODER|SHADER_RESOURCE + SHARED_NTHANDLE)。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoDecoderOutputView> outputView;
        // Copy 模式:私有解码纹理(解码写入) + 共享输出纹理(VideoProcessor 拷贝后交渲染)。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> decodeTexture;
        Microsoft::WRL::ComPtr<ID3D11VideoDecoderOutputView> decodeOutputView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> vpInputView;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> vpOutputView;
        // 渲染端打开缓存(渲染读的纹理:ZeroCopy=texture, Copy=sharedTexture)。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> renderTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1> planeYSrv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1> planeUvSrv;
        ID3D11Device* renderDeviceCached = nullptr;   // 设备失效检测
        int index = -1;
    };

    Slot* acquireFreeSlot();
    // epoch 池:returnSlot 携带 picture 创建时的池代次,池重建/销毁后旧 picture
    // 的 returnSlot 直接丢弃,避免向 freeSlots 塞已释放的槽位(悬垂 UAF)。
    void returnSlot(Slot* s, uint32_t epoch);
    int slotPoolSize();

private:
    friend class D3D11Av1Accelerator;

    bool ensureInitialized();            // 解码设备 + videoDevice/videoContext
    bool createDecodeDevice();
    // 按序列头尺寸建 ID3D11VideoDecoder(选 AV1 profile GUID)+ 槽位池。
    bool recreateDecoder(int width, int height, media::VideoCodecProfile profile);
    bool ensureSlots(int width, int height, const GUID& decoderGuid);
    // 建一个槽位(单共享纹理 / 拷贝路径私有+共享纹理)。slot 复用旧对象,先释放旧资源。
    bool createSlotOne(Slot& slot, int index, int width, int height, const GUID& decoderGuid);
    // 探测驱动是否支持单共享纹理直解码(BIND_DECODER|SHADER_RESOURCE + SHARED_NTHANDLE)。
    bool probeSingleTextureSupport();
    // 共享纹理打开到渲染设备 + 建 NV12 两平面 SRV。
    bool openSharedTexForRender(ID3D11Texture2D* sharedTexture,
                                Microsoft::WRL::ComPtr<ID3D11Texture2D>& renderTexture,
                                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1>& planeYSrv,
                                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1>& planeUvSrv);
    void destroySlots();

    // 解码路径:默认零拷贝;运行时失败(解码错误/设备移除)切到拷贝路径(VideoProcessor),
    // 参考 Chromium d3d11_copying_texture_wrapper:解码进私有纹理 -> VideoProcessorBlt 拷到共享纹理。
    enum class DecodePath { ZeroCopy, Copy };
    DecodePath decodePath = DecodePath::ZeroCopy;
    // 拷贝路径:VideoProcessor 把私有解码纹理拷到共享输出纹理。
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnumerator;
    int vpWidth = 0, vpHeight = 0;   // VideoProcessor 当前尺寸(变化时重建)
    media::VideoCodecProfile currentProfile = media::AV1PROFILE_PROFILE_MAIN;
    // 零拷贝失败 -> 重建为拷贝路径(打日志)。失败返回 false。
    bool switchToCopyMode();
    // 建 VideoProcessor/Enumerator(拷贝路径)。尺寸变化时重建。
    bool ensureVideoProcessor(int width, int height);

    bool outputFrame(Slot* slot, const media::AV1Picture& pic);

    Microsoft::WRL::ComPtr<ID3D11Device> decodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> decodeContext;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoDecoder> videoDecoder;
    Microsoft::WRL::ComPtr<ID3D11Query> copyQuery;   // 解码完成查询(交付前等完整帧)
    Microsoft::WRL::ComPtr<ID3D11Device> renderDevice;   // QRhi 渲染设备(注入)

    std::vector<std::unique_ptr<Slot>> slots;
    // 无锁槽位池(RAII 回池;渲染线程 returnSlot 与解码线程 acquireFreeSlot)。
    // 池的 epoch/重建受 poolMutex 保护:returnSlot 校验 epoch 后入队,避免悬垂。
    moodycamel::ConcurrentQueue<Slot*> freeSlots;
    std::atomic<uint32_t> poolEpoch{0};   // 池代次:重建/销毁槽位池时递增,旧 picture 回池失效
    std::mutex poolMutex;
    std::atomic<int> slotCount{0};   // 诊断:槽位总数
    int codedWidth = 0, codedHeight = 0;

    std::unique_ptr<media::AV1Decoder> av1Decoder;   // Chromium 移植的 AV1 驱动
    std::deque<std::pair<uint32_t, int64_t>> metaQueue;  // 输入 {rtp, renderMs}

    bool initialized = false;
    webrtc::DecodedImageCallback* decodeCallback = nullptr;
    std::function<void(std::shared_ptr<VideoFrame>)> onDisplayHandle;   // 直投目标(工厂注入)
    std::mutex mutex;
    // 等渲染设备注入(首帧可能早于 widget 初始化;不超时丢帧,否则 WebRTC 报错卡流)。
    std::condition_variable renderCv;
    std::atomic<bool> released{false};   // Release() 置位,唤醒等待中的 Decode
    WebrtcVideoDecoderFactory* ownerFactory = nullptr;
    int32_t streamId = 0;
};

} // namespace rtc
} // namespace hope
