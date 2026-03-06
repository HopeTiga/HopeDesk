#include "VideoTrackSourceImpl.h"

#include <string>

namespace hope {

	namespace rtc {
	
		webrtc::MediaSourceInterface::SourceState VideoTrackSourceImpl::state() const { return kLive; }

		bool VideoTrackSourceImpl::remote() const { return false; }

		bool VideoTrackSourceImpl::is_screencast() const { return true; }

		std::optional<bool> VideoTrackSourceImpl::needs_denoising() const  { return false; }

		void VideoTrackSourceImpl::PushFrame(const webrtc::VideoFrame& frame) {

			OnFrame(frame);

		}

		VideoTrackSourceImpl::VideoTrackSourceImpl():webrtc::AdaptedVideoTrackSource(1)
		{
		}

	}

}