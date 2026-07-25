#include "WebrtcD3D11TextureBuffer.h"

namespace hope {
    namespace rtc {

        WebrtcD3D11TextureBuffer::WebrtcD3D11TextureBuffer(
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

        WebrtcD3D11TextureBuffer::~WebrtcD3D11TextureBuffer() {
            if (releaseFlag && releaseFlag->load()) {
                releaseFlag->store(false);
            }
        }

        HANDLE WebrtcD3D11TextureBuffer::GetSharedHandle() const {
            return sharedHandle;
        }

        ID3D11Texture2D* WebrtcD3D11TextureBuffer::GetTexture() const {
            return texture;
        }

        webrtc::scoped_refptr<webrtc::I420BufferInterface> WebrtcD3D11TextureBuffer::ToI420() {
            return nullptr;
        }

        webrtc::scoped_refptr<const webrtc::I420BufferInterface> WebrtcD3D11TextureBuffer::ToI420() const {
            return nullptr;
        }

        webrtc::VideoFrameBuffer::Type WebrtcD3D11TextureBuffer::type() const {
        
			return Type::kNative;

        }

        int WebrtcD3D11TextureBuffer::width()const {
        
            return widths;

        }

        int WebrtcD3D11TextureBuffer::height() const {
        
            return heights;

        }

        void WebrtcD3D11TextureBuffer::FreeSharedSlot() {
            if (releaseFlag && releaseFlag->load()) {
                releaseFlag->store(false);
                releaseFlag = nullptr; // 防止析构时重复释放
            }
        }

    } // namespace rtc
} // namespace hope