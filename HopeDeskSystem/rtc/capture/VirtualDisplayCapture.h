#pragma once

// ============================================================================
// VirtualDisplayCapture — capture backend for the ZakoVDD (HopeDesk) virtual
// display driver. Drop-in replacement for the DXGI-based ScreenCapture and the
// abandoned Alluno VDD backend.
//
// The driver creates a virtual monitor; DWM composites the desktop into its
// swap chain and the driver re-publishes each frame as a keyed-mutex shared
// GPU texture. This class opens the driver's frame channel and delivers frames:
//   * gpuPath (cpuPath == false, default): hands a keyed-mutex shared handle to
//     the consumer. The consumer (NVENC) opens it on the same render adapter and
//     syncs: AcquireSync(1, INFINITE) -> encode -> ReleaseSync(0). The frame is
//     first GPU-copied into a local persistent shared texture so the same image
//     can be re-published on static desktops (repeat frame, 100 ms).
//   * cpuPath == true: delivers a mapped BGRA CPU buffer (valid during the call).
//
// Headless hosts (no physical monitor) are supported: applyTopology() activates
// the virtual display as the only/primary display. With a physical monitor the
// primary is mirrored (cloned) onto the virtual display by default so the
// captured frames show the host's main screen.
// ============================================================================

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <vector>

#include "zako-vdd/vdd_control_ioctl.h"
#include "VddChannelSync.h"

namespace hope {
namespace rtc {

// Producer-side metadata block. Must stay ABI-compatible with the driver's
// SharedFrameMetadata (128 bytes). See ZakoVDD/Rendering/SharedFrameExporter.cpp.
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
        UINT bitsPerChannel = 8;    // 8 / 10 / 12
        UINT hdrMode       = 0;     // 0 = SDR, 1 = HDR10, 2 = HDR10+
        bool cpuPath       = false; // false = GPU shared handle, true = mapped CPU buffer

        // true (default): mirror the physical primary onto the virtual display so
        // the captured frames show the host's main screen. false: the virtual
        // display is its own screen (Sunshine model). Ignored on headless hosts,
        // where the virtual display always becomes the primary.
        bool mirrorPrimary = true;

        // Find-or-create identity. On initialize() an existing display whose
        // device string matches "HPD" is reused; otherwise a new one is created.
        GUID        monitorGuid  = {};
        const char* id           = nullptr; // e.g. webrtcManagerConfig.systemService
        const char* name         = "HopeDesk Virtual Display";
        bool        removeOnDestroy = false; // remove a display we created on destruction
    };

    // GPU path. sharedHandle is a keyed-mutex DXGI shared handle (published with
    // key 1). Consumer syncs: AcquireSync(1, INFINITE) -> encode -> ReleaseSync(0).
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

    // 与下游编码器共享的通道同步状态。下游在 keyed-mutex AcquireSync 失败
    // 时置 reopenRequested；捕获线程在循环里消费该标志并重开帧通道。
    void setChannelSync(std::shared_ptr<VddChannelSync> s);

    GUID getMonitorGuid() const;
    LUID getAdapterLuid() const;

private:
    void captureThreadFunc();
    bool openDriver();
    bool sendCommand(const wchar_t* cmd);
    bool enableHardwareCursor();  // keep the OS cursor out of captured frames
    bool ensureDisplay();       // find-or-create + verify client resolution
    bool applyTopology();       // activate VDD (primary on headless, else mirror)
    bool openFrameChannel();
    void closeFrameChannel();
    bool reopenFrameChannel();  // close + open（必须在捕获线程执行）
    bool initLocalDevice();
    bool readStableMetadata(ZakoFrameMetadata& out);
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
    int frameIntervalMs = 16;

    // Local D3D11 device (CPU path), on the render adapter.
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<ID3D11Device1> d3dDevice1;

    // CPU repeat-cache: a system-memory BGRA copy of the last frame.
    std::vector<uint8_t> cpuCache;
    int cpuCacheW = 0, cpuCacheH = 0, cpuCachePitch = 0;

    UINT64 lastFrameId = 0;
    UINT64 lastDeliverTick = 0;
    bool  haveFrame = false;

    Config config;
    std::atomic<bool> capturing{ false };
    std::thread captureThread;
    GpuDataHandle gpuDataHandle;
    DataHandle dataHandle;
    std::shared_ptr<VddChannelSync> channelSync;

    // 捕获线程主动检测驱动重建（ChannelGeneration，即 MetadataSequence 高 16 位）
    // 时的基线。驱动重建后可能不再 signal 旧 frameReadyEvent，只靠事件等待会
    // 静默定格（无日志），因此每圈轮询 metadata 对比该值。
    UINT16 lastChannelGen = 0;
};

} // namespace rtc
} // namespace hope
