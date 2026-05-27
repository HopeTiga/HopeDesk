#pragma once

#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
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
            bool InitNvenc(int width, int height, uint32_t bitrateBps);
            void ProcessOutputThread();

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

            // 环形缓冲区中的输入输出指针（已预分配）
            std::vector<NV_ENC_INPUT_PTR>  mappedResources;   // D3D 纹理映射结果
            std::vector<NV_ENC_INPUT_PTR>  swInputBuffers;    // 系统内存输入缓冲区（池化）
            std::vector<HANDLE>            encodeEvents;      // 异步完成事件

            uint32_t bufCount = 0;

            // 轻量级同步原语
            std::mutex apiMutex;                  // 仅保护单次 NVENC API 调用
            std::mutex queueMutex;                // 配合条件变量
            std::condition_variable queueCond;

            // 原子索引
            std::atomic<uint32_t> writeIdx{ 0 };    // 生产者：下一个写入槽位
            std::atomic<uint32_t> readIdx{ 0 };     // 消费者：下一个读取槽位

            // 输出线程
            std::thread outputThread;
            std::atomic<bool> isRunning{ false };

            // 编码分辨率
            int widths = 0;
            int heights = 0;

            // 码率控制防抖
            std::chrono::steady_clock::time_point lastRateChangeTime;
        };
    }
}