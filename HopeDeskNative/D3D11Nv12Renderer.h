#pragma once

// =============================================================================
//  D3D11Nv12Renderer —— 裸 D3D11 画 NV12(绕过 QRhi 纹理绑定限制)。
//
//  Qt QRhi 无法给 NV12 纹理建 SRV(QRhiTexture::Format 没有 NV12),而
//  D3D11.1 的 CreateShaderResourceView1 可按 plane slice 给 NV12 建
//  R8(Y)/RG8(UV) 两个 SRV。本类用 beginExternal 注入裸 D3D11 命令,
//  在 QRhi 后缓冲 RTV 上直接画 NV12,实现零像素拷贝渲染。
// =============================================================================

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace hope {
namespace rtc {

class D3D11Nv12Renderer {
public:
    D3D11Nv12Renderer() = default;
    ~D3D11Nv12Renderer();

    D3D11Nv12Renderer(const D3D11Nv12Renderer&) = delete;
    D3D11Nv12Renderer& operator=(const D3D11Nv12Renderer&) = delete;

    // 用 QRhi 的 D3D11 设备/上下文初始化(编译 shader、建管线资源)。可重复调用(重建)。
    bool init(ID3D11Device* device, ID3D11DeviceContext* context);

    // 在 rtv(当前后缓冲)上全屏画一帧 NV12。必须在 QRhi beginExternal() 之后调用。
    // outWidth/outHeight 是后缓冲(视口)尺寸,视频纹理以 0..1 uv 拉伸铺满。
    void draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
              ID3D11ShaderResourceView* srvY, ID3D11ShaderResourceView* srvUV,
              int outWidth, int outHeight);

    void release();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState;
    bool ready = false;
};

} // namespace rtc
} // namespace hope
