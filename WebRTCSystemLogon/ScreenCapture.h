// --- START OF FILE ScreenCapture.h ---

#pragma once

#include <d3d11.h>
#include <d3d11_1.h> // 新增
#include <dxgi1_2.h>
#include <dxgi1_5.h> // 新增
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include "WinLogon.h"
#include "Utils.h"

#include <d3d11_1.h>  // 解决 ID3D11Device1, ID3D11DeviceContext1
#include <dxgi1_5.h>  // 解决 IDXGIOutput5

namespace hope {
    namespace rtc {

        // Defined to match Shader layout
        static const int YUV_BUFFERS = 4; // Increase buffer count to allow Encoder time to hold one

        struct YuvStagingBuffer {
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            std::atomic<bool> isBusy{ false }; // True if WebRTC is holding it
            uint8_t* mappedData = nullptr;     // Cache the pointer
            D3D11_MAPPED_SUBRESOURCE mappedSubresource{};
        };

        class ScreenCapture {
        public:
            struct CaptureConfig {
                int width = 1920;
                int height = 1080;
                bool enableGPUYUV = true;
                bool enableDirtyRects = true;
            };

            // Callback receives: data ptr, width, height, and a pointer to the busy flag
            using DataHandle = std::function<void(const uint8_t*, int, int, std::atomic<bool>*)>;

            ScreenCapture();
            ~ScreenCapture();

            bool initialize();
            bool startCapture();
            void stopCapture();
            void setConfig(CaptureConfig c) { config = c; }
            void setDataHandle(DataHandle dh) { dataHandle = dh; }

        private:
            void captureThreadFunc();
            bool initializeDXGI();
            bool initializeGPUConverter();
            void releaseResources();
            void releaseResourceDXGI();

            bool captureFrame();
            bool processFrame(ID3D11Texture2D* texture);
            bool processFrameCPU_BGRA(ID3D11Texture2D* texture);
            bool processFrameGPU_YUV(ID3D11Texture2D* texture);

            // ... Dirty Rect Functions (same as before) ...
            void ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture);
            void ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture);
            std::vector<RECT> MergeDirtyRects(RECT* rects, UINT count);
            void handleCaptureError(HRESULT hr);

            CaptureConfig config;
            std::atomic<bool> capturing{ false };
            std::thread captureThread;
            DataHandle dataHandle;

            // DXGI Resources
            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
            Microsoft::WRL::ComPtr<ID3D11Device1> d3dDevice1;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> d3dContext1;

            Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
            Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
            Microsoft::WRL::ComPtr<IDXGIOutput5> dxgiOutput5;
            Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dxgiDuplication;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTextures[YUV_BUFFERS]; // For CPU Path
            HANDLE sharedHandle = nullptr;

            // GPU YUV Resources
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvComputeShader;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvOutputBuffer;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> yuvUAV;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvConstantBuffer;

            // Optimized Staging Buffers
            YuvStagingBuffer yuvStagingBuffers[YUV_BUFFERS];
            int currentYuvIdx = 0;
            int currentTexture = 0; // For CPU path

            // Helpers
            std::unique_ptr<struct DirtyRegionTracker> dirtyTracker;
            std::unique_ptr<WinLogon> winLogonSwitcher;
            bool isOnWinLogonDesktop = false;
            bool desktopSwitchInProgress = false;
            int invalidCallCount = 0;
            int invalidCallDxgi = 0;
        };

    }
}