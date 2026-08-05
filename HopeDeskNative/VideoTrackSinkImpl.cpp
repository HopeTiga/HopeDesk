#include "VideoTrackSinkImpl.h"
#include "WebrtcManager.h"
#include "WebrtcD3D11TextureBuffer.h"
#include "Utils.h"

namespace hope {
namespace rtc {

VideoTrackSinkImpl::VideoTrackSinkImpl(WebrtcManager* webrtcManager) : webrtcManager(webrtcManager) {
}

VideoTrackSinkImpl::~VideoTrackSinkImpl() {
}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame& frame) {
    if (!webrtcManager || !webrtcManager->onVideoFrameHandler) return;

    // 硬解(kNative/kNV12)已由解码器直投 widget,此处跳过;本 sink 只处理软解 I420。
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> videoFrameBuffer = frame.video_frame_buffer();
    if (videoFrameBuffer &&
        (videoFrameBuffer->type() == webrtc::VideoFrameBuffer::Type::kNative ||
         videoFrameBuffer->type() == webrtc::VideoFrameBuffer::Type::kNV12))
        return;

    std::shared_ptr<VideoFrame> videoFrame = std::make_shared<VideoFrame>();
    videoFrame->width = frame.width();
    videoFrame->height = frame.height();

    // 软解:I420,数据在 RAM,需 Upload
    videoFrame->format = FrameFormat::I420;
    videoFrame->buffer = videoFrameBuffer ? videoFrameBuffer->ToI420() : nullptr;

    webrtcManager->onVideoFrameHandler(videoFrame);
}


}
}