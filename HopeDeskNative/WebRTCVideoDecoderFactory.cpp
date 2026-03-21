#include "WebRTCVideoDecoderFactory.h"

#include <media/base/media_constants.h>
#include <modules/video_coding/codecs/h264/include/h264.h>
#include <modules/video_coding/codecs/vp8/include/vp8.h>
#include <modules/video_coding/codecs/vp9/include/vp9.h>
#include <modules/video_coding/codecs/av1/dav1d_decoder.h>

#include "De265Decoder.h"
#include "Utils.h"

namespace hope {

	namespace rtc{
	
		WebRTCVideoDecoderFactory::WebRTCVideoDecoderFactory():internalDecoderFactory(new webrtc::InternalDecoderFactory()) {
		
		}

        std::unique_ptr<webrtc::VideoDecoder> WebRTCVideoDecoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {
            if (!format.IsCodecInList(GetSupportedFormats())) {

                LOG_WARNING("Trying to create decoder for unsupported format: %s", format.ToString().c_str());

                return nullptr;
            }

            if (absl::EqualsIgnoreCase(format.name, webrtc::kVp8CodecName))
                return CreateVp8Decoder(env);
            if (absl::EqualsIgnoreCase(format.name, webrtc::kVp9CodecName))
                return webrtc::VP9Decoder::Create();
            if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName))
                return webrtc::H264Decoder::Create();

            if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName)) {
                return webrtc::CreateDav1dDecoder(env);
            }

            if (absl::EqualsIgnoreCase(format.name, "H265") || absl::EqualsIgnoreCase(format.name, "HEVC")) {
                LOG_INFO("Create H265 Decoder");

                return std::make_unique<De265Decoder>();

            }

            return nullptr;
        }

        webrtc::VideoDecoderFactory::CodecSupport WebRTCVideoDecoderFactory::QueryCodecSupport(
            const webrtc::SdpVideoFormat& format,
            bool reference_scaling) const {

            LOG_INFO("format Support:%s", format.name.c_str());

            if (format.name == "H265" || format.name == "HEVC") {

                CodecSupport codecSupport;

                codecSupport.is_supported = true;

                codecSupport.is_power_efficient = true;

                return codecSupport;
            }

            return internalDecoderFactory->QueryCodecSupport(format,
                reference_scaling);
        }

        std::vector<webrtc::SdpVideoFormat> WebRTCVideoDecoderFactory::GetSupportedFormats() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalDecoderFactory->GetSupportedFormats();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }

	}

}
