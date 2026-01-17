#include "WebRTCManagerI420Buffer.h"

namespace hope {
    namespace rtc {

        WebRTCManagerI420Buffer::WebRTCManagerI420Buffer(const uint8_t* data, int width, int height, std::atomic<bool>* releaseFlag)
            : bufferWidth(width), bufferHeight(height), releaseFlag(releaseFlag) {

            // Calculate offsets for I420 (packed tightly by Shader)
            const int ySize = width * height;
            const int uvSize = ((width + 1) / 2) * ((height + 1) / 2);

            dataY = data;
            dataU = data + ySize;
            dataV = data + ySize + uvSize;

            strideY = width;
            strideU = (width + 1) / 2;
            strideV = (width + 1) / 2;
        }

        WebRTCManagerI420Buffer::~WebRTCManagerI420Buffer() {
            // Signal ScreenCapture that this buffer is free to use
            if (releaseFlag) {
                releaseFlag->store(false);
            }
        }

        int WebRTCManagerI420Buffer::width() const {
            return bufferWidth;
        }

        int WebRTCManagerI420Buffer::height() const {
            return bufferHeight;
        }

        const uint8_t* WebRTCManagerI420Buffer::DataY() const {
            return dataY;
        }

        const uint8_t* WebRTCManagerI420Buffer::DataU() const {
            return dataU;
        }

        const uint8_t* WebRTCManagerI420Buffer::DataV() const {
            return dataV;
        }

        int WebRTCManagerI420Buffer::StrideY() const {
            return strideY;
        }

        int WebRTCManagerI420Buffer::StrideU() const {
            return strideU;
        }

        int WebRTCManagerI420Buffer::StrideV() const {
            return strideV;
        }

    } // namespace rtc
} // namespace hope