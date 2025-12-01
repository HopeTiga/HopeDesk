#include "DataChannelObserverImpl.h"

#include "WebRTCManager.h"

#include "Logger.h"

namespace hope {

	namespace rtc {

		DataChannelObserverImpl::DataChannelObserverImpl(WebRTCManager * manager ):manager(manager) {
		}
	
		// The data channel state have changed.
		void DataChannelObserverImpl::OnStateChange() {
		}
		//  A data buffer was successfully received.
		void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer) {
		
			if (buffer.size() == 0) {
			
				Logger::getInstance()->error("DataChannelObserverImpl::OnMessag webrtc::DataBuffer size : 0");

				return;
			}

			if (buffer.size() > 1024 * 1024) {
			
				Logger::getInstance()->error("DataChannelObserverImpl::OnMessag webrtc::DataBuffer Exceeds the size limit");

				return;
			
			}

			manager->handleDataChannelData(reinterpret_cast<const unsigned char *>(buffer.data.data()),buffer.data.size());

		}

	}

}