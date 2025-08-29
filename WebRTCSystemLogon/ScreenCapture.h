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

// Windows and DirectX - After Boost.Asio
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// Concurrent queue
#include "concurrentqueue.h"

// WinLogon switcher
#include "WinLogon.h"

// Frame data structure
struct CapturedFrame {
    std::vector<uint8_t> bgraData;
    int width;
    int height;
    int stride;
    std::chrono::steady_clock::time_point timestamp;

    CapturedFrame() = default;
    CapturedFrame(int w, int h, int s)
        : width(w), height(h), stride(s),
        bgraData(s* h),
        timestamp(std::chrono::steady_clock::now()) {
    }
};

class ScreenCapture {
public:
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, int width, int height)>;

    struct CaptureConfig {
        int width = 0;
        int height = 0;
        int fps = 60;
        UINT outputNum = 0;
    };

    ScreenCapture();
    ~ScreenCapture();

    bool initialize();
    bool startCapture();
    void stopCapture();
    void setFrameCallback(FrameCallback callback) { frameCallback = callback; }

private:
    // Initialization methods
    bool initializeDXGI();
    void initializeFramePool(size_t poolSize);

    // Capture and processing
    void captureThreadFunc();
    bool captureFrame();
    bool processFrame(ID3D11Texture2D* texture);
    void encodeFrame(std::shared_ptr<CapturedFrame> frame);
    bool convertBGRAToYUV420(const uint8_t* bgraData, int stride, std::vector<uint8_t>& yuvBuffer);

    // Frame pool management
    std::shared_ptr<CapturedFrame> getFrameFromPool();
    void returnFrameToPool(std::shared_ptr<CapturedFrame> frame);

    // Resource cleanup
    void releaseResources();

    void releaseResourceDXGI();

    void processUpdateRegions(DXGI_OUTDUPL_FRAME_INFO* frameInfo);
    std::vector<RECT> dirtyRects;

    // DirectX resources
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dxgiDuplication;

    // OBS-style buffer management
    static constexpr int NUM_BUFFERS = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures[NUM_BUFFERS];
    bool textureReady[NUM_BUFFERS] = { false };
    bool textureMapped[NUM_BUFFERS] = { false };
    int currentTexture = 0;
    int copyWaitCount = 0;

    std::vector<uint8_t> yuvBuffer;

    // Thread management
    std::thread captureThread;
    std::thread encoderThread;
    std::atomic<bool> capturing{ false };
    std::atomic<bool> encoderRunning{ false };

    // Boost.Asio for producer-consumer pattern
    boost::asio::io_context encoderContext;
    boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code) > encoderChannel;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type >> encoderWorkGuard;

    // Frame queue and pool
    moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> frameQueue;
    moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> framePool;
    static constexpr size_t MAX_QUEUE_SIZE = 10;

    std::unique_ptr<WinLogon> winLogonSwitcher;
    std::atomic<bool> isOnWinLogonDesktop{ false };
    std::atomic<bool> desktopSwitchInProgress{ false };

    // Configuration
    CaptureConfig config;
    FrameCallback frameCallback;

    // Frame rate control
    std::chrono::milliseconds frameInterval;
    std::chrono::steady_clock::time_point lastFrameTime;

    std::vector<uint8_t> lastFrameData;
};