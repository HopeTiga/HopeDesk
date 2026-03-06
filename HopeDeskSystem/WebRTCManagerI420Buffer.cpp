#include "WebRTCManagerI420Buffer.h"

namespace hope {
    namespace rtc {

        WebRTCManagerI420Buffer::WebRTCManagerI420Buffer(const uint8_t* data, int width, int height, std::atomic<bool>* releaseFlag, int stride)
            : bufferWidth(width), bufferHeight(height), releaseFlag(releaseFlag) {

            // 如果传入的 stride 是 0 (防御性编程)，则回退到 width
            if (stride == 0) stride = width;

            // 1. 设置 Y 平面的步长为显卡实际步长
            strideY = stride;

            // 2. 这里的 U/V 步长计算假设数据是紧凑的 I420
            // 注意：如果你传的是 NV12 数据，这里其实不对，但为了先跑通流程保持原样
            strideU = (width + 1) / 2;
            strideV = (width + 1) / 2;

            // 3. 计算指针偏移
            // 【关键修改】：使用 stride * height 来跳过 Y 平面
            // 之前是用 width * height，导致读到了 Padding 区域 (黑白原因)
            const int ySize = stride * height;

            // U 平面大小 (I420 标准)
            const int uSize = strideU * ((height + 1) / 2);

            dataY = data;
            dataU = data + ySize;         // 正确跳过包含 Padding 的 Y 平面
            dataV = data + ySize + uSize; // V 平面在 U 平面之后

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