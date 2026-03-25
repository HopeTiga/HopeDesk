#pragma once

#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include "Nvenc.h"

namespace hope {
    namespace rtc {

        class NvencAV1Encoder : public webrtc::VideoEncoder {
        public:
            NvencAV1Encoder();
            ~NvencAV1Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings, const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            bool InitD3D11();
            bool InitNvenc(int width, int height, uint32_t bitrateBps);
            bool GetEncodedPacket(bool finalize); // 参照 OBS 的 get_encoded_packet

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

            // 状态控制 (完全对照 OBS 成员)
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
        };
    }
}