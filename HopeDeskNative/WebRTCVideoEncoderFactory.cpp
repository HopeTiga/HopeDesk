#include "WebRTCVideoEncoderFactory.h"

#include <media/engine/simulcast_encoder_adapter.h>

#include "Utils.h"
#include "WebRTCVideoDecoderFactory.h"

namespace hope {

	namespace rtc
	{

        WebRTCVideoEncoderFactory::WebRTCVideoEncoderFactory() :internalEncoderFactory(new webrtc::InternalEncoderFactory()) {
        
        }

        std::unique_ptr<webrtc::VideoEncoder> WebRTCVideoEncoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {

            if (format.IsCodecInList(
                internalEncoderFactory->GetSupportedFormats())) {
                return std::make_unique<webrtc::SimulcastEncoderAdapter>(
                    env,
                    /*primary_factory=*/internalEncoderFactory.get(),
                    /*fallback_factory=*/nullptr, format);
            }

            return nullptr;
        }

        webrtc::VideoEncoderFactory::CodecSupport WebRTCVideoEncoderFactory::QueryCodecSupport(
            const webrtc::SdpVideoFormat& format,
            std::optional<std::string> scalability_mode) const {

            LOG_INFO("format Support:%s",format.name.c_str());

            if (format.name == "H265" || format.name == "HEVC") {

                CodecSupport codecSupport;

                codecSupport.is_supported = true;

                codecSupport.is_power_efficient = true;

                return codecSupport;
            }

            return internalEncoderFactory->QueryCodecSupport(format,
                scalability_mode);
        }

        std::vector<webrtc::SdpVideoFormat> WebRTCVideoEncoderFactory::GetSupportedFormats() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalEncoderFactory->GetSupportedFormats();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }

        std::vector<webrtc::SdpVideoFormat> WebRTCVideoEncoderFactory::GetImplementations() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalEncoderFactory->GetImplementations();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }
	}

}
