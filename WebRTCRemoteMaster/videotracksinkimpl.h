#ifndef VIDEOTRACKSINKIMPL_H
#define VIDEOTRACKSINKIMPL_H

#include <api/media_stream_interface.h>

namespace hope{

namespace rtc{

class WebRTCManager;

// 在 WebRTCManager.h 中添加
class VideoTrackSinkImpl : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit VideoTrackSinkImpl(WebRTCManager* manager);

    // 接收视频帧的回调
    void OnFrame(const webrtc::VideoFrame& frame) override ;

private:
    WebRTCManager* manager;
};

}

}

#endif // VIDEOTRACKSINKIMPL_H
