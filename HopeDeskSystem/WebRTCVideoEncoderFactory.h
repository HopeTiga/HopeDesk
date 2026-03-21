#pragma once
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/simulcast_encoder_adapter.h"

namespace hope {

	namespace rtc {
	
		class WebRTCVideoEncoderFactory : public webrtc::VideoEncoderFactory {

		public:

			WebRTCVideoEncoderFactory();

			std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,const webrtc::SdpVideoFormat& format) override;

			webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
				const webrtc::SdpVideoFormat& format,
				std::optional<std::string> scalability_mode) const;

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

			std::vector<webrtc::SdpVideoFormat> GetImplementations() const override;

		public:

			int webrtcEnableNvidia = 0;

		private:

			const std::unique_ptr<VideoEncoderFactory> internalEncoderFactory;

		};

	}

}

