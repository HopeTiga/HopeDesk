#include "SetDescriptionObserverImpl.h"
#include "Logger.h"

namespace hope {

	namespace rtc {
	
		// SetLocalDescriptionObserver实现
		void SetLocalDescriptionObserver::OnSuccess() {
		}

		void SetLocalDescriptionObserver::OnFailure(webrtc::RTCError error) {
			Logger::getInstance()->error("SetLocalDescription failed: " + std::string(error.message()));
		}

		// SetRemoteDescriptionObserver实现
		void SetRemoteDescriptionObserver::OnSuccess() {
		}

		void SetRemoteDescriptionObserver::OnFailure(webrtc::RTCError error) {
			Logger::getInstance()->error("SetRemoteDescription failed: " + std::string(error.message()));
		}

	}
}