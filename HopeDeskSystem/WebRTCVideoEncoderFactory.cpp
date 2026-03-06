#include "WebRTCVideoEncoderFactory.h"

#include <media/engine/simulcast_encoder_adapter.h>

#include "Utils.h"

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


        std::vector<webrtc::SdpVideoFormat> WebRTCVideoEncoderFactory::GetSupportedFormats(){
            return internalEncoderFactory->GetSupportedFormats();
        }

        webrtc::VideoEncoderFactory::CodecSupport WebRTCVideoEncoderFactory::QueryCodecSupport(
            const webrtc::SdpVideoFormat& format,
            std::optional<std::string> scalability_mode) const {
            return internalEncoderFactory->QueryCodecSupport(format,
                scalability_mode);
        }

        std::vector<webrtc::SdpVideoFormat> WebRTCVideoEncoderFactory::GetSupportedFormats() const {
            return internalEncoderFactory->GetSupportedFormats();
        }

        std::vector<webrtc::SdpVideoFormat> WebRTCVideoEncoderFactory::GetImplementations() const {
            return internalEncoderFactory->GetImplementations();
        }
	}

}
