#pragma once
#include <d3d11.h>
#include <functional>
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
                std::atomic<bool>* releaseFlag)
                : texture(texture),
                sharedHandle(sharedHandle),
                frameWidth(width),
                frameHeight(height),
                releaseFlag(releaseFlag) {
            }

            ~WebRTCD3D11TextureBuffer() override {
                if (releaseFlag && releaseFlag->load()) {
                    releaseFlag->store(false);
                }
            }

            Type type() const override {
                return Type::kNative;
            }

            int width() const override { return this->frameWidth; }
            int height() const override { return this->frameHeight; }

            HANDLE GetSharedHandle() const { return sharedHandle; }
            ID3D11Texture2D* GetTexture() const { return texture; }

            webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
                return nullptr;
            }

            webrtc::scoped_refptr<const webrtc::I420BufferInterface> ToI420() const {
                return nullptr;
            }

            void FreeSharedSlot() {
                if (releaseFlag && releaseFlag->load()) {
                    releaseFlag->store(false);
                    releaseFlag = nullptr; // 置空，防止析构函数重复触发
                }
            }

        private:
            ID3D11Texture2D* texture;
            HANDLE sharedHandle;
            int frameWidth;
            int frameHeight;
            std::atomic<bool>* releaseFlag;
        };

    }
}