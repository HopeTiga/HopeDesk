#include "videotracksinkimpl.h"
#include "webrtcmanager.h"
#include <cstring> // for memcpy

namespace hope {
namespace rtc {

VideoTrackSinkImpl::VideoTrackSinkImpl(WebRTCManager* manager) : manager(manager) {
}

void VideoTrackSinkImpl::OnFrame(const webrtc::VideoFrame &frame)
{
    if (!manager || !manager->videoFrameCallback) return;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer =
        frame.video_frame_buffer()->ToI420();

    int width = frame.width();
    int height = frame.height();

    int chromaWidth = (width + 1) / 2;
    int chromaHeight = (height + 1) / 2;

    size_t ySize = width * height;
    size_t uSize = chromaWidth * chromaHeight;
    size_t vSize = chromaWidth * chromaHeight;
    size_t totalSize = ySize + uSize + vSize;

    auto videoFrame = std::make_shared<VideoFrame>(width, height);

    if (!videoFrame->data) {
        videoFrame->data = std::shared_ptr<uint8_t[]>(new uint8_t[totalSize]);
    }

    uint8_t* dstData = videoFrame->data.get();

    const uint8_t* srcY = i420Buffer->DataY();
    const uint8_t* srcU = i420Buffer->DataU();
    const uint8_t* srcV = i420Buffer->DataV();

    int strideY = i420Buffer->StrideY();
    int strideU = i420Buffer->StrideU();
    int strideV = i420Buffer->StrideV();

    uint8_t* dstY = dstData;
    uint8_t* dstU = dstData + ySize;
    uint8_t* dstV = dstData + ySize + uSize;

    if (strideY == width) {
        fastCopy(dstY, srcY, ySize);
    } else {
        for (int i = 0; i < height; ++i) {
            fastCopy(dstY + i * width, srcY + i * strideY, width);
        }
    }

    // Copy U Plane
    if (strideU == chromaWidth) {
        fastCopy(dstU, srcU, uSize);
    } else {
        for (int i = 0; i < chromaHeight; ++i) {
            fastCopy(dstU + i * chromaWidth, srcU + i * strideU, chromaWidth);
        }
    }

    // Copy V Plane
    if (strideV == chromaWidth) {
        fastCopy(dstV, srcV, vSize);
    } else {
        for (int i = 0; i < chromaHeight; ++i) {
            fastCopy(dstV + i * chromaWidth, srcV + i * strideV, chromaWidth);
        }
    }

    // 5. 触发回调
    manager->videoFrameCallback(videoFrame);
}

}
}
