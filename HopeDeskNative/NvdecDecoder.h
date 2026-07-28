#pragma once

// =============================================================================
//  NvdecDecoder —— NVIDIA NVDEC(CUVID)硬件解码器(H264 / H265 / AV1)
//
//  CUVID 解码器对三种 codec 几乎同一套流程,只差 cudaVideoCodec 枚举与 parser codec。
//  输出:NV12 设备帧 -> cuMemcpyDtoH 回主机 -> 手工 NV12->I420 对齐 -> NV12Buffer -> 回调。
//  注:NVDEC 只能通过 CUVID API 驱动,而 CUVID 建在 CUDA 驱动之上,
//      因此必须建一个 CUDA context —— 这是 NVIDIA 的接口要求,不可省略。
// =============================================================================

#include "api/video_codecs/video_decoder.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"

#include "Nvdec.h"

#include <d3d11.h>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace hope {
    namespace rtc {

        class NvdecDecoder : public webrtc::VideoDecoder {
        public:
            enum class Codec { H264, H265, AV1 };

            explicit NvdecDecoder(Codec codecType);
            ~NvdecDecoder() override;

            NvdecDecoder(const NvdecDecoder&) = delete;
            NvdecDecoder& operator=(const NvdecDecoder&) = delete;

            bool Configure(const Settings& settings) override;
            int32_t Decode(const webrtc::EncodedImage& inputImage,
                           bool missingFrames,
                           int64_t renderTimeMs) override;

            int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override;

            int32_t Release() override;

            DecoderInfo GetDecoderInfo() const override;

        private:
            // CUVID 解析器回调(pUserData == this)
            static int CUDAAPI OnVideoSequence(void* userData, CUVIDEOFORMAT* format);
            static int CUDAAPI OnPictureDecode(void* userData, CUVIDPICPARAMS* pictureParams);
            static int CUDAAPI OnPictureDisplay(void* userData, CUVIDPARSERDISPINFO* displayInfo);

            bool EnsureContext();              // 加载 NvdecApi + cuInit + 选 NVIDIA 设备 + cuCtxCreate + ctxLock
            bool ReinitDecoder(const CUVIDEOFORMAT* format); // 序列变化时重建解码器
            void EmitFrame(int pictureIndex, int64_t timestamp);
            // parser/decoder 进入坏状态(如 cuvidParseVideoData 返回 999)时重建,
            // 保留 CUDA context + ctxLock;下一个关键帧的 OnVideoSequence 会重建 decoder。
            void Flush();
            // (重新)创建 CUVID parser(Configure/Flush 复用)
            bool RecreateParser();

            Codec codecType;
            cudaVideoCodec cuvidCodec;

            NvdecApi nvdecApi{};
            CUcontext cudaContext = nullptr;       // CUVID 解码用
            CUvideoctxlock contextLock = nullptr;
            CUdevice nvDevice = 0;            // 选中的 NVIDIA 设备
            CUvideoparser videoParser = nullptr;
            CUvideodecoder videoDecoder = nullptr;

            int codedWidth = 0;    // coded 尺寸(解码表面/缓冲)
            int codedHeight = 0;
            int displayWidth = 0;  // 显示尺寸(NV12 输出,裁掉 coded padding)
            int displayHeight = 0;
            bool contextReady = false;

            webrtc::DecodedImageCallback* decodeCallback = nullptr;
            std::mutex mutex;

            // 临时主机 NV12 缓冲(按 pitch*codedHeight*3/2 复用,避免每帧分配)
            std::vector<uint8_t> hostNv12;

            // pictureIndex -> {rtp, renderMs},供 display 回调还原时间戳
            struct PictureMeta { uint32_t rtp = 0; int64_t renderMs = 0; bool used = false; };
            std::unordered_map<int, PictureMeta> pictureMeta;

            // Decode 入口暂存,OnPictureDecode 按 CurrPicIdx 落档
            uint32_t pendingRtp = 0;
            int64_t pendingRenderMs = 0;
        };

    } // namespace rtc
} // namespace hope