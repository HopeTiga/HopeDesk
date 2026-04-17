#pragma once

#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include <thread>             // 新增
#include <atomic>             // 新增
#include <condition_variable> // 新增
#include "Nvenc.h"

namespace hope {
    namespace rtc {

        class NvencH265Encoder : public webrtc::VideoEncoder {
        public:
            NvencH265Encoder();
            ~NvencH265Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            bool InitD3D11();
            bool InitNvenc(int width, int height, uint32_t bitrateBps);
            void ProcessOutputThread(); // 新增：替代原来的 GetEncodedPacket

            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;
            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;

            void* nvencSession = nullptr;
            NV_ENCODE_API_FUNCTION_LIST nvencFuncs = { NV_ENCODE_API_FUNCTION_LIST_VER };
            HMODULE nvVideoCodecHandle = nullptr;

            // 配置参数
            NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };

            std::vector<NvBitstream> bitstreams;
            std::vector<NvInputTexture> inputPool;
            std::unordered_map<HANDLE, NvInputTexture> resourceCache;
            std::vector<NV_ENC_INPUT_PTR> mappedResources;
            std::vector<NV_ENC_INPUT_PTR> swInputBuffers;
            std::vector<HANDLE> encodeEvents; // 新增：用于异步事件监听

            // 状态控制
            uint32_t bufCount = 0;
            uint32_t curBitstream = 0;
            uint32_t nextBitstream = 0;
            uint32_t buffersQueued = 0;

            std::deque<int64_t> dtsList;
            std::vector<uint8_t> header;
            bool firstPacket = true;

            std::mutex nvencApiMutex;

            // 新增：异步线程控制
            std::thread outputThread;
            std::atomic<bool> isRunning{ false };
            std::mutex queueMutex;
            std::condition_variable queueCond;

            int widths = 0;
            int heights = 0;

            std::chrono::steady_clock::time_point lastRateChangeTime;
        };
    }
}