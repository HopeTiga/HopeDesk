#pragma once

#include <d3d11.h>
#include <atomic>
#include "api/video/video_frame_buffer.h"

namespace hope {
    namespace rtc {

        class WebrtcD3D11TextureBuffer : public webrtc::VideoFrameBuffer {
        public:
            WebrtcD3D11TextureBuffer(
                HANDLE sharedHandle,
                int width,
                int height,
                std::atomic<bool>* releaseFlag);

            ~WebrtcD3D11TextureBuffer() override;

            Type type() const override;
            int width() const override;
            int height() const override;

            HANDLE GetSharedHandle() const;

            webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
            webrtc::scoped_refptr<const webrtc::I420BufferInterface> ToI420() const;

            void FreeSharedSlot();

        private:
            HANDLE sharedHandle;
            int widths;
            int heights;
            std::atomic<bool>* releaseFlag;
        };

    } // namespace rtc
} // namespace hope