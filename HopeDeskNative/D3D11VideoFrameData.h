#pragma once

// D3D11 硬解产出一帧的渲染端数据:NV12 纹理(已打开到 QRhi 设备)+ 两平面 SRV。
// keepAlive 持有 picture 引用,保证槽位在渲染完成前不被复用(RAII 回池)。

#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

namespace hope {
namespace rtc {

struct D3D11VideoFrameData {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture;   // DXGI_FORMAT_NV12,QRhi 设备
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> planeYSrv;   // plane0: R8_UNORM
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> planeUvSrv;  // plane1: R8G8_UNORM
    // 帧持有解码器 picture 的引用:保证槽位在渲染完成前不被复用(DPB 语义)。
    // 内容是一个 shared_ptr<webrtc::scoped_refptr<media::AV1Picture>>。
    std::shared_ptr<void> keepAlive;
    int width = 0;
    int height = 0;
};

} // namespace rtc
} // namespace hope
