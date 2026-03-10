#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <api/video/video_frame_buffer.h>

namespace hope {
    namespace rtc {

        class WebRTCManagerNV12Buffer : public webrtc::NV12BufferInterface {
        public:

            // 构造私有，强制走工厂
            WebRTCManagerNV12Buffer(const uint8_t* data,
                int width,
                int height,
                std::atomic<bool>* releaseFlag,
                int stride);

            ~WebRTCManagerNV12Buffer() override;

            /* ============= NV12BufferInterface ============= */
            const uint8_t* DataY() const override;
            const uint8_t* DataUV() const override;
            int StrideY() const override;
            int StrideUV() const override;

            /* ============= VideoFrameBuffer ============= */
            int width() const override;
            int height() const override;
            Type type() const override;

            webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;

        private:

            const uint8_t* dataY;
            const uint8_t* dataUV;  // NV12 里 UV 交错块首地址
            int strideY;
            int strideUV;
            int bufferWidth;
            int bufferHeight;

            std::atomic<bool>* releaseFlag;
        };

    }  // namespace rtc
}  // namespace hope