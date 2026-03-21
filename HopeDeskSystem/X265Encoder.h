#pragma once
#include "api/video_codecs/video_encoder.h"
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <x265/x265.h>
#include <vector>

namespace hope {
    namespace rtc {

        class X265Encoder : public webrtc::VideoEncoder {
        public:
            X265Encoder();
            ~X265Encoder() override;

            int InitEncode(const webrtc::VideoCodec* codecSettings,
                const webrtc::VideoEncoder::Settings& settings) override;
            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
            int Release() override;
            int Encode(const webrtc::VideoFrame& frame,
                const std::vector<webrtc::VideoFrameType>* frameTypes) override;
            void SetRates(const RateControlParameters& parameters) override;
            webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

        private:
            webrtc::EncodedImageCallback* encodedImageCallback = nullptr;

            x265_encoder* x265EncoderInstance = nullptr;
            x265_param* x265Param = nullptr;
            x265_picture* pictureIn = nullptr;

            std::vector<uint8_t> encodedBuffer;
        };
    }
}