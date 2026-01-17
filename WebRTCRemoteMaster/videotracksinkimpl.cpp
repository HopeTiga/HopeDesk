#include "videotracksinkimpl.h"
#include "WebRTCManager.h"
#include "Utils.h" // 确保包含 VideoFrame 的定义

namespace hope {
namespace rtc {

VideoTrackSinkImpl::VideoTrackSinkImpl(WebRTCManager* manager) : manager(manager) {
}

VideoTrackSinkImpl::~VideoTrackSinkImpl() {
}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame& frame) {

    if (!manager || !manager->videoFrameCallback) return;

    auto videoFrame = std::make_shared<VideoFrame>(frame);

    manager->videoFrameCallback(videoFrame);
}


}
}
