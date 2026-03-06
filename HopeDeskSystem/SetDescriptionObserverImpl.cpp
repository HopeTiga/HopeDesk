#include "SetDescriptionObserverImpl.h"
#include "Utils.h"

namespace hope {

	namespace rtc {
	
		// SetLocalDescriptionObserver实现
		void SetLocalDescriptionObserver::OnSuccess() {
		}

		void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
			LOG_ERROR("SetLocalDescription failed: %s" ,error.message());
		}

		// SetRemoteDescriptionObserver实现
		void SetRemoteDescriptionObserver::OnSuccess() {
		}

		void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
			LOG_ERROR("SetRemoteDescription failed: %s", error.message());
		}

	}
}