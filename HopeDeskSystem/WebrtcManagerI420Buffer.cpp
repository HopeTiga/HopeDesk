#include "WebrtcManagerI420Buffer.h"

namespace hope {
    namespace rtc {

        WebrtcManagerI420Buffer::WebrtcManagerI420Buffer(const uint8_t* data, int width, int height, std::atomic<bool>* releaseFlag, int stride)
            : bufferWidth(width), bufferHeight(height), releaseFlag(releaseFlag) {

            if (stride == 0) stride = width;

            strideY = stride;

            strideU = (width + 1) / 2;
            strideV = (width + 1) / 2;
            const int ySize = stride * height;

            const int uSize = strideU * ((height + 1) / 2);

            dataY = data;
            dataU = data + ySize;       
            dataV = data + ySize + uSize; 
        }

        WebrtcManagerI420Buffer::~WebrtcManagerI420Buffer() {

            if (releaseFlag) {

                releaseFlag->store(false);

            }
        }

        int WebrtcManagerI420Buffer::width() const {
            return bufferWidth;
        }

        int WebrtcManagerI420Buffer::height() const {
            return bufferHeight;
        }

        const uint8_t* WebrtcManagerI420Buffer::DataY() const {
            return dataY;
        }

        const uint8_t* WebrtcManagerI420Buffer::DataU() const {
            return dataU;
        }

        const uint8_t* WebrtcManagerI420Buffer::DataV() const {
            return dataV;
        }

        int WebrtcManagerI420Buffer::StrideY() const {
            return strideY;
        }

        int WebrtcManagerI420Buffer::StrideU() const {
            return strideU;
        }

        int WebrtcManagerI420Buffer::StrideV() const {
            return strideV;
        }

    } // namespace rtc
} // namespace hope