#pragma once

#include "api/video_codecs/video_encoder.h"
#include "api/video/video_frame_buffer.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include "Nvenc.h"

namespace hope {
    namespace rtc {

        class NvencH264Encoder : public webrtc::VideoEncoder {
        public:
            NvencH264Encoder();
            ~NvencH264Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings,
                const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(
                webrtc::EncodedImageCallback* callback) override;
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame,
                const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            bool InitD3D11();
            bool InitNvenc(int width, int height, uint32_t bitrateBps, uint32_t maxFramerate);
            bool GetEncodedPacket(bool finalize);

            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;

            Microsoft::WRL::ComPtr<ID3D11Device>        d3dDevice;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;

            void* nvencSession = nullptr;
            NV_ENCODE_API_FUNCTION_LIST nvencFuncs = { NV_ENCODE_API_FUNCTION_LIST_VER };
            HMODULE nvVideoCodecHandle = nullptr;

            NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };

            // 资源池
            std::vector<NvBitstream>       bitstreams;
            std::vector<NvInputTexture>    inputPool;
            std::unordered_map<HANDLE, NvInputTexture> resourceCache;

            std::vector<NV_ENC_INPUT_PTR>  mappedResources;
            std::vector<NV_ENC_INPUT_PTR>  swInputBuffers;

            // 直注路径下，每个 bitstream 槽位在途输入的同步状态：
            // NVENC 直接读共享纹理，keyed mutex 必须持有到 nvEncUnmapInputResource 之后
            struct PendingInput {
                Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;          // 持有到 unmap
                webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer; // 保活 d3dBuffer 以便 FreeSharedSlot
                bool isShared = false;                                // 仅直注路径为 true
            };
            std::vector<PendingInput> pendingInputs;

            // 状态控制 (参照 AV1)
            uint32_t bufCount = 0;
            uint32_t curBitstream = 0;
            uint32_t nextBitstream = 0;
            uint32_t buffersQueued = 0;

            std::deque<int64_t> dtsList;

            std::mutex nvencApiMutex;
            int widths = 0;
            int heights = 0;

            std::chrono::steady_clock::time_point lastRateChangeTime;
        };
    }
}