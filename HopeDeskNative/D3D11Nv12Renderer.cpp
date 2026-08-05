#include "D3D11Nv12Renderer.h"
#include "Utils.h"

#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace hope {
namespace rtc {

namespace {

// 顶点:全屏 quad,6 顶点(两个三角形)。右三角 + 左三角,覆盖整个屏幕。
static const float kQuadVertices[] = {
    // pos(xy)      uv
    -1.0f,  1.0f,  0.0f, 0.0f,   // 左上
     1.0f, -1.0f,  1.0f, 1.0f,   // 右下
     1.0f,  1.0f,  1.0f, 0.0f,   // 右上
    -1.0f,  1.0f,  0.0f, 0.0f,   // 左上
    -1.0f, -1.0f,  0.0f, 1.0f,   // 左下
     1.0f, -1.0f,  1.0f, 1.0f,   // 右下
};

const char* kVertexShaderSrc =
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }\n";

// 与 res/video_nv12.frag 相同的 YUV->RGB 系数:Y 全范围,UV 减 0.5 中心。
const char* kPixelShaderSrc =
    "Texture2D texY : register(t0);\n"
    "Texture2D texUV : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "float4 main(VSOut i) : SV_Target {\n"
    "    float y = texY.Sample(samp, i.uv).r;\n"
    "    float2 uv = texUV.Sample(samp, i.uv).rg - 0.5;\n"
    "    float r = y + 1.402 * uv.y;\n"
    "    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;\n"
    "    float b = y + 1.772 * uv.x;\n"
    "    return float4(r, g, b, 1.0);\n"
    "}\n";

} // namespace

D3D11Nv12Renderer::~D3D11Nv12Renderer() {
    release();
}

bool D3D11Nv12Renderer::init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    release();
    if (!dev || !ctx) return false;
    device = dev;

    HRESULT hr = S_OK;
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    hr = D3DCompile(kVertexShaderSrc, strlen(kVertexShaderSrc), nullptr, nullptr, nullptr,
                    "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                    0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Nv12Renderer] VS compile failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    hr = D3DCompile(kPixelShaderSrc, strlen(kPixelShaderSrc), nullptr, nullptr, nullptr,
                    "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                    0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Nv12Renderer] PS compile failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) ||
        FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps))) {
        LOG_ERROR("[D3D11Nv12Renderer] Create shaders failed");
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 2 * sizeof(float), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(dev->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout))) {
        LOG_ERROR("[D3D11Nv12Renderer] CreateInputLayout failed");
        return false;
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(kQuadVertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{ kQuadVertices };
    if (FAILED(dev->CreateBuffer(&vbDesc, &vbData, &vertexBuffer))) {
        LOG_ERROR("[D3D11Nv12Renderer] CreateBuffer failed");
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sampDesc, &samplerState))) {
        LOG_ERROR("[D3D11Nv12Renderer] CreateSamplerState failed");
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&blendDesc, &blendState))) {
        LOG_ERROR("[D3D11Nv12Renderer] CreateBlendState failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&rastDesc, &rasterState))) {
        LOG_ERROR("[D3D11Nv12Renderer] CreateRasterizerState failed");
        return false;
    }

    LOG_INFO("[D3D11Nv12Renderer] init done");
    ready = true;
    return true;
}

void D3D11Nv12Renderer::draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                             ID3D11ShaderResourceView* srvY, ID3D11ShaderResourceView* srvUV,
                             int outWidth, int outHeight) {
    if (!ready || !ctx || !rtv || !srvY || !srvUV) return;

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)outWidth, (float)outHeight, 0.0f, 1.0f };

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rasterState.Get());
    ctx->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(inputLayout.Get());
    UINT stride = 4 * sizeof(float);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    ID3D11ShaderResourceView* srvs[2] = { srvY, srvUV };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[1] = { samplerState.Get() };
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->Draw(6, 0);

    // 解除 SRV 绑定,避免影响 QRhi 后续状态。
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSrvs);
}

void D3D11Nv12Renderer::release() {
    vs.Reset();
    ps.Reset();
    inputLayout.Reset();
    vertexBuffer.Reset();
    samplerState.Reset();
    blendState.Reset();
    rasterState.Reset();
    device.Reset();
    ready = false;
}

} // namespace rtc
} // namespace hope
