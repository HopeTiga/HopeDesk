#include "VideoTrackSinkImpl.h"
#include "WebRTCManager.h"
#include "Utils.h"

namespace hope {
namespace rtc {

VideoTrackSinkImpl::VideoTrackSinkImpl(WebRTCManager* manager) : manager(manager) {
}

VideoTrackSinkImpl::~VideoTrackSinkImpl() {
}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame& frame) {

    if (!manager || !manager->onVideoFrameHandler) return;

    auto videoFrame = std::make_shared<VideoFrame>(frame);

    manager->onVideoFrameHandler(videoFrame);
}


}
}
