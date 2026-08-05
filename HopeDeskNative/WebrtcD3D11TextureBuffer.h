#pragma once

#include "D3D11VideoFrameData.h"
#include "api/video/video_frame_buffer.h"

namespace hope {
namespace rtc {

// 承载 D3D11 解码产出的 NV12 纹理(kNative buffer)。
// 解码器和渲染器在同一适配器上各有一个 D3D11 设备,纹理经 NT 共享句柄交接:
//   - nv12 : DXGI_FORMAT_NV12 纹理(已打开到渲染设备,同一块 GPU 显存)
//   - srvY : plane0 R8     (Y 平面)
//   - srvUV: plane1 RG8    (UV 交错)
// 渲染端(QRhi)画完后再释放 D3D11VideoFrameData,忙标志复位回解码器池。
class WebrtcD3D11TextureBuffer : public webrtc::VideoFrameBuffer {
public:
    explicit WebrtcD3D11TextureBuffer(std::shared_ptr<D3D11VideoFrameData> data);

    ~WebrtcD3D11TextureBuffer() override;

    Type type() const override { return Type::kNative; }
    int width() const override { return data ? data->width : 0; }
    int height() const override { return data ? data->height : 0; }

    std::shared_ptr<D3D11VideoFrameData> GetFrameData() const { return data; }

    // 把帧数据转给 app 的 VideoFrame;此后本 buffer 析构不再释放帧数据。
    std::shared_ptr<D3D11VideoFrameData> DetachData() {
        auto d = std::move(data);
        data.reset();
        return d;
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override { return nullptr; }

private:
    std::shared_ptr<D3D11VideoFrameData> data;
};

} // namespace rtc
} // namespace hope
