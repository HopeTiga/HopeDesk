#include "WebRTCVideoEncoderFactory.h"

#include <media/engine/simulcast_encoder_adapter.h>

#include "NvencAV1Encoder.h"
#include "NvencH265Encoder.h"
#include "X265Encoder.h"
#include "Utils.h"
#include "WebRTCVideoDecoderFactory.h"

namespace hope {

	namespace rtc
	{

        WebRTCVideoEncoderFactory::WebRTCVideoEncoderFactory() :internalEncoderFactory(new webrtc::InternalEncoderFactory()) {
        
        }

        std::unique_ptr<webrtc::VideoEncoder> WebRTCVideoEncoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {

            if ((format.name == "AV1" || format.name == "av1") && webrtcEnableNvidia == 1) {
     
                LOG_INFO("NvencAV1Encoder");

                return std::make_unique<NvencAV1Encoder>();

            }

            if ((format.name == "H265" || format.name == "h265") && webrtcEnableNvidia == 1) {

                LOG_INFO("NvencH2651Encoder");

                return std::make_unique<NvencH265Encoder>();

            }
            else if (format.name == "H265" || format.name == "h265") {
            
                LOG_INFO("X265Encoder");

				return std::make_unique<X265Encoder>();

            }

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

            LOG_INFO("format Support:%s",format.name);

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
