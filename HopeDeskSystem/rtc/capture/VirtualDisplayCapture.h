#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <chrono>
#include <vector>

#include "zako-vdd/vdd_control_ioctl.h"
#include "VddChannelSync.h"

namespace hope {
namespace rtc {

// Must stay ABI-compatible with the driver's SharedFrameMetadata (128 bytes).
struct ZakoFrameMetadata {
    UINT32 Magic;                    // 'ZVDF' = 0x5A564446
    UINT32 Version;
    UINT32 Width;
    UINT32 Height;
    UINT32 DxgiFormat;
    UINT32 IsHdr;
    float  MaxNits, MinNits, MaxFALL;
    UINT64 FrameCounter;
    UINT64 LastPresentQpc;
    UINT64 LastPublishQpc;
    UINT32 LastPresentationFrameNumber;
    UINT32 LastDirtyRectCount;
    UINT64 ReplacedUnreadFrames;
    UINT64 DroppedConsumerHeldFrames;
    UINT64 DroppedAcquireFailures;
    UINT32 MetadataSize;
    UINT32 SlotCount;
    UINT32 SlotIndex;
    UINT32 MetadataSequence;         // high16 = channel generation, low1 = write bit
    UINT32 AdapterLuidLowPart;
    INT32  AdapterLuidHighPart;
    UINT64 ProducerQpcFrequency;
};
static_assert(sizeof(ZakoFrameMetadata) == 128, "ZakoFrameMetadata ABI mismatch");

class VirtualDisplayCapture {
public:
    struct Config {
        int  width         = 1920;
        int  height        = 1080;
        int  refreshRate   = 144;    // Hz
        bool cpuPath       = false; // false = GPU shared handle, true = mapped CPU buffer

        bool mirrorPrimary = true;   // 忽略于无物理显示器的主机

        GUID        monitorGuid  = {};
        const char* id           = nullptr; // e.g. webrtcManagerConfig.systemService
        const char* name         = "HopeDesk Virtual Display";
        bool        removeOnDestroy = false; // remove a display we created on destruction
    };

    // GPU path. Consumer syncs: AcquireSync(1, 100ms) -> encode -> ReleaseSync(0).
    using GpuDataHandle = std::function<void(
        HANDLE sharedHandle,
        int width, int height,
        UINT format, UINT rowPitch,
        UINT64 frameId)>;

    // CPU path. data points to mapped BGRA pixels, valid only during the call.
    using DataHandle = std::function<void(
        const uint8_t* data,
        int width, int height,
        int rowPitch,
        UINT64 frameId)>;

    VirtualDisplayCapture();
    ~VirtualDisplayCapture();

    bool initialize();   // open driver, find-or-create display, activate topology, open frame channel
    bool startCapture();
    void stopCapture();

    void setConfig(Config c);
    void setGpuDataHandle(GpuDataHandle h);
    void setDataHandle(DataHandle h);

    void setChannelSync(std::shared_ptr<VddChannelSync> s);

    GUID getMonitorGuid() const;

private:
    void captureThreadFunc();
    bool openDriver();
    bool reopenDriver();  // 关闭旧 device handle 后重新 CreateFileW，触发 PnP 唤醒（D3→D0）
    bool sendCommand(const wchar_t* cmd);
    bool enableHardwareCursor();  // keep the OS cursor out of captured frames
    bool ensureDisplay();       // find-or-create + verify client resolution
    bool applyTopology();       // activate VDD (primary on headless, else mirror)
    bool openFrameChannel(int maxAttempts = 30);   // maxAttempts 限制 NOT_READY 重试时长（30×500ms 留给启动）
    void closeFrameChannel();
    bool reopenFrameChannel();  // close + open（必须在捕获线程执行，有界重试）

    // 非破坏性探测的结果。驱动在模式切换窗口内会合法地对开通道请求回 NOT_READY，
    // 那不是驱动僵死，不能据此重载设备。
    enum class ProbeResult {
        Ready,      // 读到 generation
        NotReady,   // 驱动侧模式切换 / 纹理重建中，可恢复
        Rejected,   // 请求被驱动拒绝（LUID 不符、参数非法），重载设备修不了
    };
    ProbeResult probeChannelGeneration(UINT16& outGen);
    bool initLocalDevice();
    bool readStableMetadata(ZakoFrameMetadata& out);
    bool isMetadataValid(const ZakoFrameMetadata& meta) const;
    bool deliverNewFrame(const ZakoFrameMetadata& meta);
    void deliverRepeatFrame();

    // Driver / frame channel state
    HANDLE driverDevice = INVALID_HANDLE_VALUE;
    HANDLE frameReadyEvent = nullptr;
    HANDLE metaMapping = nullptr;
    ZakoFrameMetadata* pMeta = nullptr;
    UINT32 slotCount = 0;
    std::vector<HANDLE> slotHandles;                 // sealed NT handles from the response
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> slotTex;
    std::vector<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>> slotKm;
    LUID adapterLuid{};
    GUID monitorGuid{};
    bool weCreated = false;

    // Local D3D11 device (CPU path), on the render adapter.
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<ID3D11Device1> d3dDevice1;

    // CPU repeat-cache: a system-memory BGRA copy of the last frame.
    std::vector<uint8_t> cpuCache;
    int cpuCacheW = 0, cpuCacheH = 0, cpuCachePitch = 0;

    UINT64 lastFrameId = 0;
    bool  haveFrame = false;

    Config config;
    std::atomic<bool> capturing{ false };
    std::thread captureThread;
    GpuDataHandle gpuDataHandle;
    DataHandle dataHandle;
    std::shared_ptr<VddChannelSync> channelSync;

    UINT16 lastChannelGen = 0;

    int   lastWidth = 0;
    int   lastHeight = 0;
    UINT  lastFormat = 0;

    // 通道-down 指数退避：open/reopen 失败后先退避再重试，避免热自旋与 15s 阻塞。
    bool channelDown = false;
    std::chrono::steady_clock::time_point nextOpenRetryAt{};
    int  openBackoffMs = 0;
    int  downLogCounter = 0;
    DWORD lastProbeError = 0;
};

} // namespace rtc
} // namespace hope
