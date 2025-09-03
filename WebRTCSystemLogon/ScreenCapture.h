#pragma once

// IMPORTANT: Define these before any Windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Boost.Asio MUST be included before Windows headers to avoid WinSock conflicts
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>

// Standard library
#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <immintrin.h>  // For _mm_pause()

// Windows and DirectX - After Boost.Asio
#include <windows.h>
#include <d3d11_1.h>  // For D3D11.1 features
#include <dxgi1_2.h>   
#include <dxgi1_5.h>   // For DXGI 1.5 features
#include <wrl/client.h>

// Concurrent queue
#include "concurrentqueue.h"

// WinLogon switcher
#include "WinLogon.h"

// Forward declarations
struct DirtyRegionTracker;
struct GPUTextureRing;

// ========== 帧数据结构 ==========
struct CapturedFrame {
    std::vector<uint8_t> bgraData;
    int width;
    int height;
    int stride;
    std::chrono::steady_clock::time_point timestamp;
    bool isYUV = false;  // 标记是否已经是YUV格式

    CapturedFrame() = default;
    CapturedFrame(int w, int h, int s)
        : width(w), height(h), stride(s),
        bgraData(s* h),
        timestamp(std::chrono::steady_clock::now()) {
    }
};

// ========== 零拷贝帧信息 ==========
struct FrameReadyInfo {
    HANDLE sharedHandle;  // 共享纹理句柄
    std::chrono::steady_clock::time_point timestamp;
    bool hasUpdates;
    std::vector<RECT> dirtyRects;  // 只包含更新的区域
};

// ========== 性能度量 ==========
struct CaptureMetrics {
    double captureTimeMs = 0;
    double processTimeMs = 0;
    double encodeTimeMs = 0;
    int droppedFrames = 0;
    int dirtyPixelCount = 0;
    int totalPixelCount = 0;
    double gpuUtilization = 0;

    void Reset() {
        captureTimeMs = 0;
        processTimeMs = 0;
        encodeTimeMs = 0;
        droppedFrames = 0;
        dirtyPixelCount = 0;
        totalPixelCount = 0;
        gpuUtilization = 0;
    }
};

class ScreenCapture {
public:
    // 回调函数类型
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, int width, int height)>;
    using GPUEncoderCallback = std::function<void(ID3D11Texture2D* texture)>;

    struct CaptureConfig {
        int width = 0;
        int height = 0;
        int fps = 60;
        UINT outputNum = 0;
        bool enableGPUEncoding = false;  // 启用GPU直接编码
        bool enableDirtyRects = true;    // 启用脏矩形优化
        bool enableZeroCopy = true;      // 启用零拷贝
    };

    ScreenCapture();
    ~ScreenCapture();

    // 基础接口
    bool initialize();
    bool startCapture();
    void stopCapture();

    // 回调设置
    void setFrameCallback(FrameCallback callback) { frameCallback = callback; }
    void setGPUEncoderCallback(GPUEncoderCallback callback) { gpuEncoderCallback = callback; }

    // 配置
    void setConfig(const CaptureConfig& cfg) { config = cfg; }
    const CaptureConfig& getConfig() const { return config; }

    // 性能监控
    CaptureMetrics getMetrics() const { return metrics; }

    // 获取共享纹理句柄（用于跨进程零拷贝）
    HANDLE getSharedTextureHandle() const { return sharedHandle; }

private:
    // ========== 初始化方法 ==========
    bool initializeDXGI();
    bool initializeGPUConverter();
    void initializeFramePool(size_t poolSize);
    bool initializeAdvancedFeatures();

    // ========== 捕获方法 ==========
    void captureThreadFunc();
    bool captureFrame();                    // 传统捕获（后备）
    bool captureFrameZeroCopy();           // 零拷贝捕获
    bool processFrame(ID3D11Texture2D* texture);

    // ========== 脏矩形处理 ==========
    void ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture);
    void ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect);
    void ProcessDirtyRect(ID3D11Texture2D* sourceTexture, const RECT* dirtyRect);
    std::vector<RECT> MergeDirtyRects(RECT* rects, UINT count);

    // ========== GPU处理 ==========
    void ProcessOnGPU(ID3D11Texture2D* sourceTexture);
    bool convertBGRAToYUV420_GPU(ID3D11Texture2D* sourceTexture, std::vector<uint8_t>& yuvBuffer);

    // ========== 编码处理 ==========
    void encodeFrame(std::shared_ptr<CapturedFrame> frame);
    bool convertBGRAToYUV420(const uint8_t* bgraData, int stride, std::vector<uint8_t>& yuvBuffer);

    // ========== 帧池管理 ==========
    std::shared_ptr<CapturedFrame> getFrameFromPool();
    void returnFrameToPool(std::shared_ptr<CapturedFrame> frame);

    // ========== 错误处理 ==========
    void handleCaptureError(HRESULT hr);
    void NotifyFrameReady();

    // ========== 资源清理 ==========
    void releaseResources();
    void releaseResourceDXGI();

    // ========== DirectX 11 资源 ==========
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<ID3D11Device1> d3dDevice1;                // D3D11.1
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> d3dContext1;        // D3D11.1
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
    Microsoft::WRL::ComPtr<IDXGIOutput5> dxgiOutput5;                // DXGI 1.5
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dxgiDuplication;

    // ========== 共享纹理（零拷贝） ==========
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex;
    HANDLE sharedHandle = nullptr;

    // ========== GPU YUV转换资源 ==========
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvComputeShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> yuvOutputBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> yuvUAV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> stagingBuffer;

    // ========== GPU编码器直接输出 ==========
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuEncoderTexture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> gpuEncoderUAV;

    // ========== 缓冲管理 ==========
    static constexpr int NUM_BUFFERS = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures[NUM_BUFFERS];
    int currentTexture = 0;

    std::unique_ptr<GPUTextureRing> gpuRing;  // GPU纹理环形缓冲
    std::unique_ptr<DirtyRegionTracker> dirtyTracker;  // 脏矩形跟踪器

    // ========== 线程管理 ==========
    std::thread captureThread;
    std::thread encoderThread;
    std::atomic<bool> capturing{ false };
    std::atomic<bool> encoderRunning{ false };
    std::atomic<bool> hasAcquiredFrame{ false };

    // ========== Boost.Asio异步管道 ==========
    boost::asio::io_context encoderContext;
    boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code)> encoderChannel;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> encoderWorkGuard;

    // ========== 队列 ==========
    moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> frameQueue;
    moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> framePool;
    moodycamel::ConcurrentQueue<FrameReadyInfo> frameReadyQueue;  // 零拷贝帧队列
    static constexpr size_t MAX_QUEUE_SIZE = 10;

    // ========== 桌面切换 ==========
    std::unique_ptr<WinLogon> winLogonSwitcher;
    std::atomic<bool> isOnWinLogonDesktop{ false };
    std::atomic<bool> desktopSwitchInProgress{ false };

    // ========== 配置和回调 ==========
    CaptureConfig config;
    FrameCallback frameCallback;
    GPUEncoderCallback gpuEncoderCallback;

    // ========== 性能控制 ==========
    std::chrono::milliseconds frameInterval;
    std::chrono::steady_clock::time_point lastFrameTime;
    CaptureMetrics metrics;

    // ========== 功能标志 ==========
    bool enableZeroCopy = true;
    bool useAdvancedFeatures = false;

    // ========== YUV缓冲区 ==========
    std::vector<uint8_t> yuvBuffer;
};