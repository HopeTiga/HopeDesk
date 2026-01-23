#pragma once
#include <functional>

#include <api/data_channel_interface.h>

namespace hope {

namespace rtc {

class WebRTCManager;

class DataChannelObserverImpl : public webrtc::DataChannelObserver {

public:

    DataChannelObserverImpl(WebRTCManager * manager);

    void setOnDataHandle(std::function<void(unsigned char*, size_t)> func);

    void setOnStateChangeHandle(std::function<void()> func);
    // The data channel state have changed.
    void OnStateChange() ;
    //  A data buffer was successfully received.
    void OnMessage(const webrtc::DataBuffer& buffer) ;

private:

    WebRTCManager* manager;

    std::function<void( unsigned char*, size_t)> onDataHandle;

    std::function<void()> onStateChangeHandle;

};


}

}

