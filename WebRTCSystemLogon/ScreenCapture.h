#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include "WinLogon.h"

namespace hope {
    namespace rtc {

        struct DirtyRegionTracker;

        class ScreenCapture {

        public:

            using DataHandle = std::function<void(const uint8_t* data, int stride, int width, int height, bool isYUV)>;

            struct CaptureConfig {
                int width = 0;
                int height = 0;
                int fps = 60;
                bool enableDirtyRects = true;
                bool enableGPUYUV = true; // 开关：是否启用 GPU 转换
            };

            ScreenCapture();
            ~ScreenCapture();

            bool initialize();
            bool startCapture();
            void stopCapture();

            void setDataHandle(DataHandle handle) { dataHandle = handle; }
            void setConfig(const CaptureConfig& cfg) { config = cfg; }
            const CaptureConfig& getConfig() const { return config; }

        private:
            bool initializeDXGI();
            bool initializeGPUConverter(); // 初始化 Shader 资源

            void captureThreadFunc();
            bool captureFrame();

            // 统一处理入口
            bool processFrame(ID3D11Texture2D* texture);

            // 分支路径
            bool processFrameCPU_BGRA(ID3D11Texture2D* texture); // 路径A: CPU 直通
            bool processFrameGPU_YUV(ID3D11Texture2D* texture);  // 路径B: GPU 转换

            void ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture);
            void ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture);
            std::vector<RECT> MergeDirtyRects(RECT* rects, UINT count);

            void handleCaptureError(HRESULT hr);
            void releaseResources();
            void releaseResourceDXGI();

            // DXGI 核心资源
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

            // --- CPU 模式下的缓冲 (BGRA Texture) ---
            static constexpr int NUM_BUFFERS = 3;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures[NUM_BUFFERS];

            // --- GPU 模式下的资源 (Compute Shader) ---
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvComputeShader;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvConstantBuffer;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvOutputBuffer;  // GPU 显存 Buffer
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvStagingBuffer; // CPU 可读 Buffer
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> yuvUAV;

            int currentTexture = 0;

            std::unique_ptr<DirtyRegionTracker> dirtyTracker;
            std::unique_ptr<WinLogon> winLogonSwitcher;

            std::thread captureThread;
            std::atomic<bool> capturing{ false };

            std::atomic<bool> isOnWinLogonDesktop{ false };
            std::atomic<bool> desktopSwitchInProgress{ false };
            int invalidCallCount = 0;
            int invalidCallDxgi = 0;

            CaptureConfig config;
            DataHandle dataHandle;

            HANDLE sharedHandle = nullptr;
        };
    }
}