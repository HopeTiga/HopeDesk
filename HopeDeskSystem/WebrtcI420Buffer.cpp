#include "WebrtcI420Buffer.h"

namespace hope {
    namespace rtc {

        WebrtcI420Buffer::WebrtcI420Buffer(const uint8_t* data, int width, int height, std::atomic<bool>* releaseFlag, int stride)
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

        WebrtcI420Buffer::~WebrtcI420Buffer() {

            if (releaseFlag) {

                releaseFlag->store(false);

            }
        }

        int WebrtcI420Buffer::width() const {
            return bufferWidth;
        }

        int WebrtcI420Buffer::height() const {
            return bufferHeight;
        }

        const uint8_t* WebrtcI420Buffer::DataY() const {
            return dataY;
        }

        const uint8_t* WebrtcI420Buffer::DataU() const {
            return dataU;
        }

        const uint8_t* WebrtcI420Buffer::DataV() const {
            return dataV;
        }

        int WebrtcI420Buffer::StrideY() const {
            return strideY;
        }

        int WebrtcI420Buffer::StrideU() const {
            return strideU;
        }

        int WebrtcI420Buffer::StrideV() const {
            return strideV;
        }

    } // namespace rtc
} // namespace hope