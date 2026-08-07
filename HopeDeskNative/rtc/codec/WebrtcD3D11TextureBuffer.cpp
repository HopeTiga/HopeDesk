#include "WebrtcD3D11TextureBuffer.h"

namespace hope {
namespace rtc {

WebrtcD3D11TextureBuffer::WebrtcD3D11TextureBuffer(std::shared_ptr<D3D11VideoFrameData> data)
    : data(std::move(data)) {
}

WebrtcD3D11TextureBuffer::~WebrtcD3D11TextureBuffer() {
    // 帧数据已转交给 VideoFrame 时(data 为空),此处不再释放。
}

} // namespace rtc
} // namespace hope
