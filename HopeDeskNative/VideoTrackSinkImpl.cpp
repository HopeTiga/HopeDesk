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

    auto videoFrame = std::make_shared<VideoFrame>();
    videoFrame->width = frame.width();
    videoFrame->height = frame.height();

    auto vfb = frame.video_frame_buffer();

    if (vfb && vfb->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
        // 硬解:NV12(NVDEC -> cuMemcpyDtoH -> NV12Buffer),数据在 RAM,需 Upload
        videoFrame->format = FrameFormat::Nv12;
        videoFrame->nv12Buffer = vfb->GetNV12();
    }
    else {
        // 软解:I420,数据在 RAM,需 Upload
        videoFrame->format = FrameFormat::I420;
        videoFrame->buffer = vfb ? vfb->ToI420() : nullptr;
    }

    webrtcManager->onVideoFrameHandler(videoFrame);
}


}
}