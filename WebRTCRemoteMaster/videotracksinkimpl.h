#pragma once
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

namespace hope {
namespace rtc {

class WebRTCManager;

class VideoTrackSinkImpl : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    VideoTrackSinkImpl(WebRTCManager* manager);
    ~VideoTrackSinkImpl() override;

    void OnFrame(const webrtc::VideoFrame& frame) override;

private:
    WebRTCManager* manager;
};

}
}
