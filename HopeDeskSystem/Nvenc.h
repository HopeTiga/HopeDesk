#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>


#include "nvEncodeAPI.h" 

// 模拟 OBS 的 bitstream 结构
struct NvBitstream {
    NV_ENC_OUTPUT_PTR ptr = nullptr;
};

// 模拟 OBS 的共享纹理缓存
struct NvInputTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;
    NV_ENC_REGISTERED_PTR regPtr = nullptr;
};
