#pragma once

#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>

#include "Nvenc.h"

namespace hope {
    namespace rtc {

        class NvencH265Encoder : public webrtc::VideoEncoder {
        public:
            NvencH265Encoder();
            ~NvencH265Encoder() override;

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

            struct RegisteredResource {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                NV_ENC_REGISTERED_PTR registeredPtr;
                NV_ENC_BUFFER_FORMAT format;
            };
            std::unordered_map<HANDLE, RegisteredResource> resourceCache;

            // 🔥 [修复] 将深度从 12 降低到 3。
            // 异步模式下 3 个 Buffer 已经足够实现流水线并行，过多的缓存会导致画面延迟甚至跳帧。
            static const int MAX_BUFFER_COUNT = 24;
            NV_ENC_OUTPUT_PTR bitstreamBuffers[MAX_BUFFER_COUNT] = { nullptr };
            NV_ENC_INPUT_PTR sysMemBuffers[MAX_BUFFER_COUNT] = { nullptr };

            HANDLE asyncEvents[MAX_BUFFER_COUNT] = { nullptr };
            NV_ENC_INPUT_PTR mappedInputBuffers[MAX_BUFFER_COUNT] = { nullptr };
            uint32_t rtpTimestamps[MAX_BUFFER_COUNT] = { 0 };
            int64_t captureTimes[MAX_BUFFER_COUNT] = { 0 };
            int encodeWidths[MAX_BUFFER_COUNT];
            int encodeHeights[MAX_BUFFER_COUNT];

            int currentBufferIdx = 0;

            std::mutex encodeMutex;
            std::condition_variable queueCond;
            std::queue<int> pendingQueue;

            std::thread encoderThread;
            std::atomic<bool> isEncoding{ false };

            webrtc::scoped_refptr<webrtc::VideoFrameBuffer> retainedBuffers[MAX_BUFFER_COUNT];

            std::mutex nvencApiMutex;
        };
    }
}