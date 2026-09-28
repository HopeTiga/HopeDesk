#pragma once

#include "api/video_codecs/video_encoder.h"
#include "api/video/video_frame_buffer.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include <memory>
#include "Nvenc.h"
#include "../capture/VddChannelSync.h"

namespace hope {
    namespace rtc {

        class NvencAV1Encoder : public webrtc::VideoEncoder {
        public:
            NvencAV1Encoder();
            ~NvencAV1Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;

            void SetChannelSync(std::shared_ptr<VddChannelSync> s);
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            bool InitD3D11();
            bool InitNvenc(int width, int height, uint32_t bitrateBps, uint32_t maxFramerate);
            bool InitVideoProcessor(int width, int height);
            bool GetEncodedPacket(bool finalize);

            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;
            Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;

            Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;
            Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnumerator;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor;

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

            // 直注路径在途输入
            struct PendingInput {
                Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;          // 持有到 unmap
                webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer; // 保活 d3dBuffer 以便 FreeSharedSlot
                bool isShared = false;                                // 仅直注路径为 true
            };
            std::vector<PendingInput> pendingInputs;

            uint32_t bufCount = 0;
            uint32_t curBitstream = 0;   // 当前提取索引
            uint32_t nextBitstream = 0;  // 下一个下发索引
            uint32_t buffersQueued = 0;  // 队列中等待的帧数
            uint32_t outputDelay = 0;    // B帧/Lookahead导致的输出延迟帧数

            std::deque<int64_t> dtsList;
            std::vector<uint8_t> header;  // 存储 SPS/PPS/VPS
            bool firstPacket = true;

            std::mutex nvencApiMutex;
            int widths = 0;
            int heights = 0;

            std::shared_ptr<VddChannelSync> channelSync;
            uint32_t lastSeenGeneration = 0;
            uint32_t lastRequestedGeneration = ~0u;

            std::chrono::steady_clock::time_point lastRateChangeTime;

            uint64_t timingFrames = 0;
            uint64_t timingWindowFrames = 0;
            double   timingGapMsSum = 0.0;
            double   timingAcquireMsSum = 0.0, timingAcquireMsMax = 0.0;
            double   timingHoldMsSum = 0.0;
            double   timingEncodeMsSum = 0.0, timingEncodeMsMax = 0.0;
            double   timingTailMsSum = 0.0;
            uint32_t timingQueuedMax = 0;
            uint64_t timingLockBusy = 0;
            double   timingBlitGpuUsSum = 0.0;
            uint64_t timingBlitSamples = 0;
            std::chrono::steady_clock::time_point timingLastFrameAt{};
            std::chrono::steady_clock::time_point timingWindowStart{};
            Microsoft::WRL::ComPtr<ID3D11Query> vpTimestampDisjoint;
            Microsoft::WRL::ComPtr<ID3D11Query> vpTimestampStart;
            Microsoft::WRL::ComPtr<ID3D11Query> vpTimestampEnd;
        };
    }
}