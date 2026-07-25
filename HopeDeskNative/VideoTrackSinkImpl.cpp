#include "VideoTrackSinkImpl.h"
#include "WebrtcManager.h"
#include "Utils.h"

namespace hope {
namespace rtc {

VideoTrackSinkImpl::VideoTrackSinkImpl(WebrtcManager* webrtcManager) : webrtcManager(webrtcManager) {
}

VideoTrackSinkImpl::~VideoTrackSinkImpl() {
}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame& frame) {

    if (!webrtcManager || !webrtcManager->onVideoFrameHandler) return;

    auto videoFrame = std::make_shared<VideoFrame>(frame);

    webrtcManager->onVideoFrameHandler(videoFrame);
}


}
}
