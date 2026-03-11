#pragma once
#define WEBRTC_WIN 1
#define RTC_DISABLE_LOGGING
#pragma warning(push)
#pragma warning(disable: 4146)
#include <system_wrappers/include/clock.h>
#include <rtc_base/numerics/divide_round.h>
#pragma warning(pop)
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

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats();

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

