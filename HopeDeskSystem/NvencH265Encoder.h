#pragma once
#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "Nvenc.h"

namespace hope {
    namespace rtc {

        struct RegisteredResource {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            NV_ENC_REGISTERED_PTR registeredPtr;
            NV_ENC_BUFFER_FORMAT format;
        };

        struct PendingTask {
            int bufIdx;
            webrtc::VideoFrame frame;
            NV_ENC_INPUT_PTR mappedInput;
            bool forceKeyFrame;

            // 添加构造函数以解决 VideoFrame 没有默认构造函数的问题
            PendingTask(int idx, const webrtc::VideoFrame& f, NV_ENC_INPUT_PTR input, bool key)
                : bufIdx(idx), frame(f), mappedInput(input), forceKeyFrame(key) {
            }
        };

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
            void FetchThreadFunc();

            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;

            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;

            void* nvencSession = nullptr;
            NV_ENCODE_API_FUNCTION_LIST nvencFuncs = { NV_ENCODE_API_FUNCTION_LIST_VER };
            HMODULE nvVideoCodecHandle = nullptr;

            NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };

            std::unordered_map<HANDLE, RegisteredResource> resourceCache;

            static const int MaxBufferCount = 12;
            NV_ENC_OUTPUT_PTR bitstreamBuffers[MaxBufferCount] = { nullptr };
            NV_ENC_INPUT_PTR sysMemBuffers[MaxBufferCount] = { nullptr };
            NV_ENC_INPUT_PTR mappedInputBuffers[MaxBufferCount] = { nullptr };
            HANDLE completionEvents[MaxBufferCount] = { nullptr };

            int currentBufferIdx = 0;
            std::mutex nvencApiMutex;
            std::mutex queueMutex;
            std::condition_variable queueCv;
            std::queue<PendingTask> pendingTaskQueue;

            std::thread fetchThread;
            std::atomic<bool> isRunning{ false };
        };
    }
}