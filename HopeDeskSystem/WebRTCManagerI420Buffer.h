#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <api/video/i420_buffer.h>

namespace hope {
    namespace rtc {

        class WebRTCManagerI420Buffer : public webrtc::I420BufferInterface {
        public:
            WebRTCManagerI420Buffer(const uint8_t* data, int width, int height, std::atomic<bool> * releaseFlag, int stride);
            ~WebRTCManagerI420Buffer() override;

            int width() const override;
            int height() const override;

            const uint8_t* DataY() const override;
            const uint8_t* DataU() const override;
            const uint8_t* DataV() const override;

            int StrideY() const override;
            int StrideU() const override;
            int StrideV() const override;

        private:
            const uint8_t* dataY;
            const uint8_t* dataU;
            const uint8_t* dataV;
            int strideY;
            int strideU;
            int strideV;
            int bufferWidth;
            int bufferHeight;
            std::atomic<bool>* releaseFlag;
        };

    } // namespace rtc
} // namespace hope