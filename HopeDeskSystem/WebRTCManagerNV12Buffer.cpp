#include "WebRTCManagerNV12Buffer.h"
#include <api/video/i420_buffer.h>  // for I420Buffer::Create

#include "Utils.h"

namespace hope {
    namespace rtc {

        /* private ctor */
        WebRTCManagerNV12Buffer::WebRTCManagerNV12Buffer(const uint8_t* data,
            int width,
            int height,
            std::atomic<bool>* releaseFlag,
            int stride)
            : bufferWidth(width),
            bufferHeight(height),
            releaseFlag(releaseFlag) {

            strideY = (stride > 0) ? stride : width;

            strideUV = strideY;  // NV12 里 UV  stride 与 Y stride 相同

            dataY = data;

            dataUV = data + static_cast<size_t>(strideY) * height;  // UV 紧挨 Y 后面

        }

        WebRTCManagerNV12Buffer::~WebRTCManagerNV12Buffer() {
            if (releaseFlag) {
            
                releaseFlag->store(false);

            }
               
        }

        /* ============= NV12BufferInterface ============= */
        const uint8_t* WebRTCManagerNV12Buffer::DataY() const {
            return dataY;
        }

        const uint8_t* WebRTCManagerNV12Buffer::DataUV() const {
            return dataUV;
        }

        int WebRTCManagerNV12Buffer::StrideY() const {
            return strideY;
        }

        int WebRTCManagerNV12Buffer::StrideUV() const {
            return strideUV;
        }

        /* ============= VideoFrameBuffer ============= */
        int WebRTCManagerNV12Buffer::width() const {
            return bufferWidth;
        }

        int WebRTCManagerNV12Buffer::height() const {
            return bufferHeight;
        }

        webrtc::VideoFrameBuffer::Type WebRTCManagerNV12Buffer::type() const {
            return webrtc::VideoFrameBuffer::Type::kNV12;
        }

        webrtc::scoped_refptr<webrtc::I420BufferInterface> WebRTCManagerNV12Buffer::ToI420() {

            webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer = webrtc::I420Buffer::Create(bufferWidth, bufferHeight);

            return i420Buffer;

        }

    }  // namespace rtc
}  // namespace hope