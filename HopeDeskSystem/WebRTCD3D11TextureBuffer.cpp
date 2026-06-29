#include "WebRTCD3D11TextureBuffer.h"

namespace hope {
    namespace rtc {

        WebRTCD3D11TextureBuffer::WebRTCD3D11TextureBuffer(
            ID3D11Texture2D* texture,
            HANDLE sharedHandle,
            int width,
            int height,
            std::atomic<bool>* releaseFlag) {
            this->texture = texture;
            this->sharedHandle = sharedHandle;
            this->widths = width;
            this->heights = height;
            this->releaseFlag = releaseFlag;
        }

        WebRTCD3D11TextureBuffer::~WebRTCD3D11TextureBuffer() {
            if (releaseFlag && releaseFlag->load()) {
                releaseFlag->store(false);
            }
        }

        HANDLE WebRTCD3D11TextureBuffer::GetSharedHandle() const {
            return sharedHandle;
        }

        ID3D11Texture2D* WebRTCD3D11TextureBuffer::GetTexture() const {
            return texture;
        }

        webrtc::scoped_refptr<webrtc::I420BufferInterface> WebRTCD3D11TextureBuffer::ToI420() {
            return nullptr;
        }

        webrtc::scoped_refptr<const webrtc::I420BufferInterface> WebRTCD3D11TextureBuffer::ToI420() const {
            return nullptr;
        }

        webrtc::VideoFrameBuffer::Type WebRTCD3D11TextureBuffer::type() const {
        
			return Type::kNative;

        }

        int WebRTCD3D11TextureBuffer::width()const {
        
            return widths;

        }

        int WebRTCD3D11TextureBuffer::height() const {
        
            return heights;

        }

        void WebRTCD3D11TextureBuffer::FreeSharedSlot() {
            if (releaseFlag && releaseFlag->load()) {
                releaseFlag->store(false);
                releaseFlag = nullptr; // 防止析构时重复释放
            }
        }

    } // namespace rtc
} // namespace hope