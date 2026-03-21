#pragma once
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "media/engine/internal_decoder_factory.h"

namespace hope {

	namespace rtc {

		class WebRTCVideoDecoderFactory : public webrtc::VideoDecoderFactory {

		public:

			WebRTCVideoDecoderFactory();

			std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;
			
			CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
				bool reference_scaling) const;

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;


		public:

			int webrtcEnableNvidia = 0;

		private:

			std::unique_ptr<webrtc::VideoDecoderFactory> internalDecoderFactory;

		};

	}

}

