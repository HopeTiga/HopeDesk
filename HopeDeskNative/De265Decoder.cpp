#include "De265Decoder.h"
#include "Utils.h" // 确保你的宏用法类似于 printf

namespace hope {
namespace rtc {

De265Decoder::De265Decoder() = default;

De265Decoder::De265Decoder(const webrtc::Environment& env)
    : cropToRenderResolution(env.field_trials().IsEnabled(
          "WebRTC-De265Decoder-CropToRenderResolution")) {}

De265Decoder::~De265Decoder() {
    Release();
}

bool De265Decoder::Configure(const Settings& settings) {
    if (decoderContext) {
        Release();
    }

    LOG_INFO("De265Decoder::Configure - Initializing libde265 context...");
    decoderContext = de265_new_decoder(); //
    if (!decoderContext) {
        LOG_ERROR("De265Decoder::Configure - Failed to allocate libde265 decoder context.");
        return false;
    }

    int numberOfThreads = std::clamp(settings.number_of_cores(), 2, kMaxDe265Threads);
    LOG_INFO("De265Decoder::Configure - Starting worker threads: %d", numberOfThreads);
    de265_error err = de265_start_worker_threads(decoderContext, numberOfThreads); //

    if (!de265_isOK(err)) { //
        LOG_WARNING("De265Decoder::Configure - Failed to start threads: %s", de265_get_error_text(err));
    }

    return true;
}

int32_t De265Decoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
    LOG_INFO("De265Decoder::RegisterDecodeCompleteCallback called.");
    decodeCompleteCallback = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t De265Decoder::Release() {
    LOG_INFO("De265Decoder::Release called.");
    if (decoderContext) {
        de265_free_decoder(decoderContext); //
        decoderContext = nullptr;
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo De265Decoder::GetDecoderInfo() const {
    DecoderInfo info;
    info.implementation_name = kDe265Name;
    info.is_hardware_accelerated = false;
    return info;
}

const char* De265Decoder::ImplementationName() const {
    return kDe265Name;
}

int32_t De265Decoder::Decode(const webrtc::EncodedImage& encodedImage,
                             int64_t /*renderTimeMs*/) {
    if (!decoderContext || decodeCompleteCallback == nullptr) {
        LOG_ERROR("De265Decoder::Decode - UNINITIALIZED! Context: %p, Callback: %p",
                  decoderContext, decodeCompleteCallback);
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    // 1. 推入裸流数据
    de265_error pushResult = de265_push_data(
        decoderContext, encodedImage.data(), encodedImage.size(),
        encodedImage.RtpTimestamp(), nullptr); //

    if (!de265_isOK(pushResult)) { //
        LOG_ERROR("De265Decoder::Decode - push_data failed: %s", de265_get_error_text(pushResult));
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    de265_push_end_of_frame(decoderContext); //

    int moreToDecode = 1;
    int loopCounter = 0;

    while (moreToDecode) {
        loopCounter++;

        // 3. 执行解码
        de265_error decodeResult = de265_decode(decoderContext, &moreToDecode); //

        // 4. 获取解码后的画面
        const de265_image* decodedImage = nullptr;

        while ((decodedImage = de265_get_next_picture(decoderContext)) != nullptr) { //

            if (de265_get_chroma_format(decodedImage) != de265_chroma_420) { //
                LOG_ERROR("De265Decoder::Decode - Unhandled chroma format. Dropping frame.");
                continue;
            }

            int width = de265_get_image_width(decodedImage, 0); //
            int height = de265_get_image_height(decodedImage, 0); //

            int strideY = 0, strideU = 0, strideV = 0;
            const uint8_t* planeY = de265_get_image_plane(decodedImage, 0, &strideY); //
            const uint8_t* planeU = de265_get_image_plane(decodedImage, 1, &strideU); //
            const uint8_t* planeV = de265_get_image_plane(decodedImage, 2, &strideV); //

            if (!planeY || !planeU || !planeV) {
                LOG_ERROR("De265Decoder::Decode - Failed to retrieve image planes. Dropping.");
                continue;
            }

            webrtc::scoped_refptr<webrtc::I420Buffer> i420Buffer = webrtc::I420Buffer::Copy(
                width, height, planeY, strideY, planeU, strideU, planeV, strideV);

            if (!i420Buffer) {
                LOG_ERROR("De265Decoder::Decode - Failed to allocate I420Buffer (OOM?). Return ERROR.");
                return WEBRTC_VIDEO_CODEC_MEMORY;
            }

            webrtc::VideoFrame decodedFrame =
                webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(i420Buffer)
                    .set_rtp_timestamp(de265_get_image_PTS(decodedImage)) //
                    .set_ntp_time_ms(encodedImage.ntp_time_ms_)
                    .set_color_space(encodedImage.ColorSpace())
                    .build();


            decodeCompleteCallback->Decoded(decodedFrame, std::nullopt, std::nullopt);

        }

        // 5. 错误处理
        // 只要不是 OK 也不是 WAITING_FOR_INPUT_DATA，往往是致命错误或者严重警告
        if (!de265_isOK(decodeResult) && decodeResult != DE265_ERROR_WAITING_FOR_INPUT_DATA) { //
            LOG_WARNING("De265Decoder::Decode - Abnormal decode state: %s. Aborting this frame.",
                        de265_get_error_text(decodeResult));
            // 重点看你的控制台有没有打印这行。如果是警告（如某些参数不受支持），可以尝试 return WEBRTC_VIDEO_CODEC_OK 强行跳过。
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace rtc
}  // namespace hope
