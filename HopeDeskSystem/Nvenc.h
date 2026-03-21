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

struct RegisteredResource {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR registeredPtr;
    NV_ENC_BUFFER_FORMAT format;
};