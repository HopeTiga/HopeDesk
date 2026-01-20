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


namespace hope {

    namespace rtc {

        static constexpr int YUV_BUFFERS = 8; 

        struct YuvStagingBuffer {
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            std::atomic<bool> isBusy{ false };
            uint8_t* mappedData = nullptr; 
            D3D11_MAPPED_SUBRESOURCE mappedSubresource{};
        };

        struct Nv12TextureBuffer {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
            std::atomic<bool> isBusy{ false };
            D3D11_MAPPED_SUBRESOURCE mappedSubresource{};
        };

        enum class CaptureLevels {

            CPU = 1,

            GPU = 1,

            PRO = 2

        };

        class ScreenCapture {
        public:
            struct CaptureConfig {
                int width = 1920;
                int height = 1080;
                CaptureLevels levels;
                CaptureLevels uselevels;
                bool enableDirtyRects = true;
            };

            using DataHandle = std::function<void(const uint8_t*, int, int, std::atomic<bool>*,int, CaptureLevels)>;

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
            bool initializeProcessor();
            void releaseResources();
            void releaseResourceDXGI();

            bool captureFrame();
            bool processFrame(ID3D11Texture2D* texture);
            bool processFrameCPU(ID3D11Texture2D* texture);
            bool processFrameGPU(ID3D11Texture2D* texture);
            bool processFramePro(ID3D11Texture2D* texture);

            void ProcessDirtyRects(DXGI_OUTDUPL_FRAME_INFO* frameInfo, ID3D11Texture2D* sourceTexture, ID3D11Texture2D* destTexture);
            void ProcessMoveRect(ID3D11Texture2D* sourceTexture, DXGI_OUTDUPL_MOVE_RECT* moveRect, ID3D11Texture2D* destTexture);
            std::vector<RECT> MergeDirtyRects(RECT* rects, UINT count);
            void handleCaptureError(HRESULT hr);

            CaptureConfig config;
            std::atomic<bool> capturing{ false };
            std::thread captureThread;
            DataHandle dataHandle;

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

            Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvComputeShader;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvOutputBuffer;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> yuvUAV;
            Microsoft::WRL::ComPtr<ID3D11Buffer> yuvConstantBuffer;
            D3D11_BUFFER_DESC bufferDesc;

            Microsoft::WRL::ComPtr<ID3D11VideoDevice> proVideoDevice; //Pro path
            Microsoft::WRL::ComPtr<ID3D11VideoContext> proVideoContext;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessor> proVideoProcessor;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> proVideoProcessorEnum;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> proOutputTex;

            YuvStagingBuffer yuvStagingBuffers[YUV_BUFFERS];
            std::vector<std::unique_ptr<YuvStagingBuffer>> emergencyBuffers;

            Nv12TextureBuffer nv12TextureBuffers[YUV_BUFFERS];
            std::vector<std::unique_ptr<Nv12TextureBuffer>> emergencyNv12Buffers;

            int currentYuvIdx = 0;
            int currentTexture = 0;

            int currentProIdx = 0;

            std::unique_ptr<struct DirtyRegionTracker> dirtyTracker;
            std::unique_ptr<WinLogon> winLogonSwitcher;
            bool isOnWinLogonDesktop = false;
            bool desktopSwitchInProgress = false;
            int invalidCallCount = 0;
        };

    }
}