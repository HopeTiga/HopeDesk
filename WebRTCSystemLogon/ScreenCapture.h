#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>

#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <immintrin.h>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>   
#include <dxgi1_5.h>
#include <wrl/client.h>

#include "concurrentqueue.h"
#include "WinLogon.h"

namespace hope {

    namespace rtc {
        struct DirtyRegionTracker;
        struct GPUTextureRing;

        struct CapturedFrame {
            std::vector<uint8_t> bgraData;
            int width;
            int height;
            int stride;
            std::chrono::steady_clock::time_point timestamp;
            bool isYUV = false;

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
            using GPUEncoderCallback = std::function<void(ID3D11Texture2D* texture)>;

            struct CaptureConfig {
                int width = 0;
                int height = 0;
                int fps = 120;
                UINT outputNum = 0;
                bool enableGPUEncoding = true;
                bool enableDirtyRects = true;
                bool enableGPUYUV = true;
            };

            ScreenCapture();
            ~ScreenCapture();

            bool initialize();
            bool startCapture();
            void stopCapture();

            void setFrameCallback(FrameCallback callback) { frameCallback = callback; }
            void setGPUEncoderCallback(GPUEncoderCallback callback) { gpuEncoderCallback = callback; }

            void setConfig(const CaptureConfig& cfg) { config = cfg; }
            const CaptureConfig& getConfig() const { return config; }

        private:
            bool initializeDXGI();
            bool initializeGPUConverter();
            void initializeFramePool(size_t poolSize);

            void captureThreadFunc();
            bool captureFrame();
            bool processFrame(ID3D11Texture2D* texture);

            void ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture);
            void ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture);
            std::vector<RECT> MergeDirtyRects(RECT* rects, UINT count);

            bool convertBGRAToYUV420_GPU(ID3D11Texture2D* sourceTexture, std::vector<uint8_t>& yuvBuffer);

            void encodeFrame(std::shared_ptr<CapturedFrame> frame);
            bool convertBGRAToYUV420(const uint8_t* bgraData, int stride, std::vector<uint8_t>& yuvBuffer);

            std::shared_ptr<CapturedFrame> getFrameFromPool();
            void returnFrameToPool(std::shared_ptr<CapturedFrame> frame);

            void handleCaptureError(HRESULT hr);

            void releaseResources();
            void releaseResourceDXGI();

            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
            Microsoft::WRL::ComPtr<ID3D11Device1> d3dDevice1;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> d3dContext1;
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
            Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
            Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
            Microsoft::WRL::ComPtr<IDXGIOutput5> dxgiOutput5;
            Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dxgiDuplication;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;

            Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvComputeShader;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvOutputBuffer;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> yuvUAV;
            Microsoft::WRL::ComPtr<ID3D11Buffer> stagingBuffer;

            static constexpr int NUM_BUFFERS = 5;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures[NUM_BUFFERS];
            int currentTexture = 0;

            std::unique_ptr<GPUTextureRing> gpuRing;
            std::unique_ptr<DirtyRegionTracker> dirtyTracker;

            std::thread captureThread;
            std::thread encoderThread;
            std::atomic<bool> capturing{ false };
            std::atomic<bool> encoderRunning{ false };

            boost::asio::io_context encoderContext;
            boost::asio::experimental::concurrent_channel
                <void(boost::system::error_code) > encoderChannel;
            std::unique_ptr<boost::asio::executor_work_guard<
                boost::asio::io_context::executor_type >> encoderWorkGuard;

            moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> frameQueue;
            moodycamel::ConcurrentQueue<std::shared_ptr<CapturedFrame>> framePool;
            static constexpr size_t MAX_QUEUE_SIZE = 10;

            std::unique_ptr<WinLogon> winLogonSwitcher;

            std::atomic<bool> isOnWinLogonDesktop{ false };
            std::atomic<bool> desktopSwitchInProgress{ false };

            CaptureConfig config;

            FrameCallback frameCallback;
            GPUEncoderCallback gpuEncoderCallback;

            bool useAdvancedFeatures = false;
            std::vector<uint8_t> yuvBuffer;

            int invalidCallCount = 0;
            int invalidCallDxgi = 0;

            HANDLE sharedHandle = nullptr;
        };
    }
}