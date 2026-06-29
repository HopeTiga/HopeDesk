#pragma once

#include <d3d11.h>
#include <atomic>
#include "api/video/video_frame_buffer.h"

namespace hope {
    namespace rtc {

        class WebRTCD3D11TextureBuffer : public webrtc::VideoFrameBuffer {
        public:
            WebRTCD3D11TextureBuffer(
                ID3D11Texture2D* texture,
                HANDLE sharedHandle,
                int width,
                int height,
                std::atomic<bool>* releaseFlag);

            ~WebRTCD3D11TextureBuffer() override;

            Type type() const override;
            int width() const override;
            int height() const override;

            HANDLE GetSharedHandle() const;
            ID3D11Texture2D* GetTexture() const;

            webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
            webrtc::scoped_refptr<const webrtc::I420BufferInterface> ToI420() const;

            void FreeSharedSlot();

        private:
            ID3D11Texture2D* texture;
            HANDLE sharedHandle;
            int widths;
            int heights;
            std::atomic<bool>* releaseFlag;
        };

    } // namespace rtc
} // namespace hope