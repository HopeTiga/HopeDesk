#pragma once

#include <media/base/adapted_video_track_source.h>


namespace hope {

	namespace rtc {

		class VideoTrackSourceImpl : public webrtc::AdaptedVideoTrackSource {

		public:

			VideoTrackSourceImpl();

			~VideoTrackSourceImpl() override = default;

			webrtc::MediaSourceInterface::SourceState state() const override;

			bool remote() const override;

			bool is_screencast() const override;

			std::optional<bool> needs_denoising() const override;

			void PushFrame(const webrtc::VideoFrame& frame);


		};
	
	}

}
