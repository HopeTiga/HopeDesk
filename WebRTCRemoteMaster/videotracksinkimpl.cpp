#include "videotracksinkimpl.h"
#include "webrtcmanager.h"

namespace hope {

namespace rtc{

VideoTrackSinkImpl::VideoTrackSinkImpl(WebRTCManager* manager):manager(manager){

}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame &frame)
{
    if (!manager) return;

    // 获取帧数据
    int width = frame.width();
    int height = frame.height();

    // 转换为I420格式
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer =
        frame.video_frame_buffer()->ToI420();

    // 创建RGB缓冲区
    auto rgbFrame = std::make_shared<VideoFrame>(width, height);

    // YUV转RGB
    manager->convertYUV420ToRGBA32(
        i420Buffer->DataY(),
        i420Buffer->DataU(),
        i420Buffer->DataV(),
        width, height,
        i420Buffer->StrideY(),
        i420Buffer->StrideU(),
        i420Buffer->StrideV(),
        rgbFrame->data.get()
        );

    // 触发回调
    if (manager->videoFrameCallback) {
        manager->videoFrameCallback(rgbFrame);
    }
}
}
}
