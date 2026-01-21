#include "DataChannelObserverImpl.h"

#include "WebRTCManager.h"

#include "Utils.h"

namespace hope {

	namespace rtc {

		DataChannelObserverImpl::DataChannelObserverImpl(WebRTCManager * manager ):manager(manager) {
		}

		void DataChannelObserverImpl::setOnDataHandle(std::function<void(const unsigned char*, size_t)> func)
		{
			this->onDataHandle = func;
		}
	
		// The data channel state have changed.
		void DataChannelObserverImpl::OnStateChange() {
		}
		//  A data buffer was successfully received.
		void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer) {
		
			if (buffer.size() == 0) {
			
				LOG_ERROR("DataChannelObserverImpl::OnMessag webrtc::DataBuffer size : 0");

				return;
			}

			if (buffer.size() > 1024 * 1024) {
			
				LOG_ERROR("DataChannelObserverImpl::OnMessag webrtc::DataBuffer Exceeds the size limit");

				return;
			
			}

			if (onDataHandle) {
			
				onDataHandle(reinterpret_cast<const unsigned char*>(buffer.data.data()), buffer.data.size());

			}

		}

	}

}