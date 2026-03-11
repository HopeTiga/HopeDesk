#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "nvEncodeAPI.h" 
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>

namespace hope {
    namespace rtc {

        struct RegisteredResource {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            NV_ENC_REGISTERED_PTR registeredPtr;
            NV_ENC_BUFFER_FORMAT format;
        };

        class NvencAV1Encoder : public webrtc::VideoEncoder {
        public:
            NvencAV1Encoder();
            ~NvencAV1Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings,
                const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame,
                const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            bool InitD3D11();
            bool InitNvenc(int width, int height, uint32_t bitrateBps);
            void ProcessOutput(); // 异步获取编码结果的工作线程

            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;

            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;

            void* nvencSession = nullptr;
            NV_ENCODE_API_FUNCTION_LIST nvencFuncs = { NV_ENCODE_API_FUNCTION_LIST_VER };
            HMODULE nvVideoCodecHandle = nullptr;

            NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };

            std::unordered_map<HANDLE, RegisteredResource> resourceCache;

            // 异步模式需要多几个 Buffer 以形成真正的并行管线，提升性能
            static const int MAX_BUFFER_COUNT = 4;
            NV_ENC_OUTPUT_PTR bitstreamBuffers[MAX_BUFFER_COUNT] = { nullptr };
            NV_ENC_INPUT_PTR sysMemBuffers[MAX_BUFFER_COUNT] = { nullptr };

            // 异步需要用到的事件句柄与资源记录
            HANDLE asyncEvents[MAX_BUFFER_COUNT] = { nullptr };
            NV_ENC_INPUT_PTR mappedInputBuffers[MAX_BUFFER_COUNT] = { nullptr };
            uint32_t rtpTimestamps[MAX_BUFFER_COUNT] = { 0 };
            int64_t captureTimes[MAX_BUFFER_COUNT] = { 0 };
            int encodeWidths[MAX_BUFFER_COUNT];   // 新增：保存宽度
            int encodeHeights[MAX_BUFFER_COUNT];  // 新增：保存高度

            int currentBufferIdx = 0;

            std::mutex encodeMutex;
            std::condition_variable queueCond;
            std::queue<int> pendingQueue; // 记录等待拿流的 Buffer 下标

            std::thread encoderThread;
            std::atomic<bool> isEncoding{ false };
        };
    }
}