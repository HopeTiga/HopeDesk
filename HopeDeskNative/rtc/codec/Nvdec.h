#pragma once

// =============================================================================
//  NVDEC (CUVID) 头 —— 无 CUDA SDK 依赖
//
//  本项目没有 cuda.h，但 cuviddec.h 第 37~39 行 #include <cuda.h>。
//  这里仿照 NVENC 的做法：动态加载 nvcuda.dll / nvcuvid.dll，并在这里
//  #define __cuda_cuda_h__ 让 cuviddec.h 跳过对 cuda.h 的包含，自行 typedef
//  所需的最小 CUDA 驱动类型。
// =============================================================================

#include <windows.h>

#ifndef __cuda_cuda_h__
#define __cuda_cuda_h__

#define CUDA_VERSION 13000          // 使 cuviddec.h 的 __CUVID_DEVPTR64 生效 (Win64 -> cuvidMapVideoFrame64)

#ifndef CUDAAPI
#define CUDAAPI __stdcall
#endif

typedef int CUresult;
#define CUDA_SUCCESS 0

struct CUctx_st;       typedef struct CUctx_st* CUcontext;
struct CUstream_st;    typedef struct CUstream_st* CUstream;
struct CUarray_st;     typedef struct CUarray_st* CUarray;
struct CUgraphicsResource_st; typedef struct CUgraphicsResource_st* CUgraphicsResource;
#if defined(_WIN64) || defined(__LP64__) || defined(__x86_64__)
typedef unsigned long long CUdeviceptr;
#else
typedef unsigned int CUdeviceptr;
#endif
typedef int CUdevice;

// CUDA memcpy / memorytype(零拷贝:cuMemcpy2D 把 NVDEC NV12 拆平面灌进 D3D11 纹理)
typedef enum { CU_MEMORYTYPE_HOST = 0x01, CU_MEMORYTYPE_DEVICE = 0x02, CU_MEMORYTYPE_ARRAY = 0x03, CU_MEMORYTYPE_UNIFIED = 0x04 } CUmemorytype;
// cuGraphicsD3D11RegisterResource Flags
#define CU_GRAPHICS_REGISTER_FLAGS_NONE         0x00
#define CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY   0x01
#define CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD 0x02  // 只写(QRhi 只读),避免 map 与 D3D11 SRV 读冲突
typedef struct CUDA_MEMCPY2D_st {
    size_t srcXInBytes, srcY;
    CUmemorytype srcMemoryType;
    const void *srcHost; CUdeviceptr srcDevice; CUarray srcArray; size_t srcPitch;
    size_t dstXInBytes, dstY;
    CUmemorytype dstMemoryType;
    void *dstHost; CUdeviceptr dstDevice; CUarray dstArray; size_t dstPitch;
    size_t WidthInBytes, Height;
} CUDA_MEMCPY2D;

// D3D11 类型前向声明(实现里 #include <d3d11.h> 拿实定义),用于 interop API 签名
struct ID3D11Device;
struct ID3D11Resource;

#endif // __cuda_cuda_h__

// nvcuvid.h 会带入 cuviddec.h（依赖上面定义的 CUDA 类型）
#include "nvcuvid.h"

// =============================================================================
//  CUDA 驱动 API 函数指针 typedef（运行时 GetProcAddress 解析）
// =============================================================================
typedef CUresult(CUDAAPI* PFN_cuInit)(unsigned int);
typedef CUresult(CUDAAPI* PFN_cuDeviceGetCount)(int*);
typedef CUresult(CUDAAPI* PFN_cuDeviceGet)(CUdevice*, int);
typedef CUresult(CUDAAPI* PFN_cuDeviceGetName)(char*, int, CUdevice);
typedef CUresult(CUDAAPI* PFN_cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
typedef CUresult(CUDAAPI* PFN_cuCtxDestroy)(CUcontext);
typedef CUresult(CUDAAPI* PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult(CUDAAPI* PFN_cuCtxPopCurrent)(CUcontext*);
typedef CUresult(CUDAAPI* PFN_cuMemAlloc)(CUdeviceptr*, size_t);
typedef CUresult(CUDAAPI* PFN_cuMemFree)(CUdeviceptr);
typedef CUresult(CUDAAPI* PFN_cuMemcpyDtoH)(void*, CUdeviceptr, size_t);
typedef CUresult(CUDAAPI* PFN_cuStreamSynchronize)(CUstream);
// CUDA-D3D11 interop(零拷贝)
typedef CUresult(CUDAAPI* PFN_cuD3D11CtxCreate)(CUcontext*, unsigned int, CUdevice, ID3D11Device*);
typedef CUresult(CUDAAPI* PFN_cuGraphicsD3D11RegisterResource)(CUgraphicsResource*, ID3D11Resource*, unsigned int);
typedef CUresult(CUDAAPI* PFN_cuGraphicsUnregisterResource)(CUgraphicsResource);
typedef CUresult(CUDAAPI* PFN_cuGraphicsMapResources)(unsigned int, CUgraphicsResource*, CUstream);
typedef CUresult(CUDAAPI* PFN_cuGraphicsUnmapResources)(unsigned int, CUgraphicsResource*, CUstream);
typedef CUresult(CUDAAPI* PFN_cuGraphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, unsigned int, unsigned int);
typedef CUresult(CUDAAPI* PFN_cuMemcpy2D)(const CUDA_MEMCPY2D*);

// =============================================================================
//  CUVID API 函数指针 typedef
// =============================================================================
typedef CUresult(CUDAAPI* PFN_cuvidCreateVideoParser)(CUvideoparser*, CUVIDPARSERPARAMS*);
typedef CUresult(CUDAAPI* PFN_cuvidParseVideoData)(CUvideoparser, CUVIDSOURCEDATAPACKET*);
typedef CUresult(CUDAAPI* PFN_cuvidDestroyVideoParser)(CUvideoparser);
typedef CUresult(CUDAAPI* PFN_cuvidCreateDecoder)(CUvideodecoder*, CUVIDDECODECREATEINFO*);
typedef CUresult(CUDAAPI* PFN_cuvidDestroyDecoder)(CUvideodecoder);
typedef CUresult(CUDAAPI* PFN_cuvidDecodePicture)(CUvideodecoder, CUVIDPICPARAMS*);
typedef CUresult(CUDAAPI* PFN_cuvidMapVideoFrame64)(CUvideodecoder, int, unsigned long long*, unsigned int*, CUVIDPROCPARAMS*);
typedef CUresult(CUDAAPI* PFN_cuvidUnmapVideoFrame64)(CUvideodecoder, unsigned long long);
typedef CUresult(CUDAAPI* PFN_cuvidCtxLockCreate)(CUvideoctxlock*, CUcontext);
typedef CUresult(CUDAAPI* PFN_cuvidCtxLockDestroy)(CUvideoctxlock);
typedef CUresult(CUDAAPI* PFN_cuvidCtxLock)(CUvideoctxlock, unsigned int);
typedef CUresult(CUDAAPI* PFN_cuvidCtxUnlock)(CUvideoctxlock, unsigned int);

// =============================================================================
//  函数表 + 加载器
// =============================================================================
struct NvdecApi {
    HMODULE cudaDll = nullptr;
    HMODULE cuvidDll = nullptr;

    PFN_cuInit                 cuInit = nullptr;
    PFN_cuDeviceGetCount       cuDeviceGetCount = nullptr;
    PFN_cuDeviceGet            cuDeviceGet = nullptr;
    PFN_cuDeviceGetName        cuDeviceGetName = nullptr;
    PFN_cuCtxCreate            cuCtxCreate = nullptr;
    PFN_cuCtxDestroy           cuCtxDestroy = nullptr;
    PFN_cuCtxPushCurrent       cuCtxPushCurrent = nullptr;
    PFN_cuCtxPopCurrent        cuCtxPopCurrent = nullptr;
    PFN_cuMemAlloc             cuMemAlloc = nullptr;
    PFN_cuMemFree              cuMemFree = nullptr;
    PFN_cuMemcpyDtoH           cuMemcpyDtoH = nullptr;
    PFN_cuStreamSynchronize    cuStreamSynchronize = nullptr;
    // CUDA-D3D11 interop(零拷贝)
    PFN_cuD3D11CtxCreate                   cuD3D11CtxCreate = nullptr;
    PFN_cuGraphicsD3D11RegisterResource    cuGraphicsD3D11RegisterResource = nullptr;
    PFN_cuGraphicsUnregisterResource       cuGraphicsUnregisterResource = nullptr;
    PFN_cuGraphicsMapResources             cuGraphicsMapResources = nullptr;
    PFN_cuGraphicsUnmapResources           cuGraphicsUnmapResources = nullptr;
    PFN_cuGraphicsSubResourceGetMappedArray cuGraphicsSubResourceGetMappedArray = nullptr;
    PFN_cuMemcpy2D                         cuMemcpy2D = nullptr;

    PFN_cuvidCreateVideoParser cuvidCreateVideoParser = nullptr;
    PFN_cuvidParseVideoData    cuvidParseVideoData = nullptr;
    PFN_cuvidDestroyVideoParser cuvidDestroyVideoParser = nullptr;
    PFN_cuvidCreateDecoder     cuvidCreateDecoder = nullptr;
    PFN_cuvidDestroyDecoder    cuvidDestroyDecoder = nullptr;
    PFN_cuvidDecodePicture     cuvidDecodePicture = nullptr;
    PFN_cuvidMapVideoFrame64   cuvidMapVideoFrame64 = nullptr;
    PFN_cuvidUnmapVideoFrame64 cuvidUnmapVideoFrame64 = nullptr;
    PFN_cuvidCtxLockCreate     cuvidCtxLockCreate = nullptr;
    PFN_cuvidCtxLockDestroy    cuvidCtxLockDestroy = nullptr;
    PFN_cuvidCtxLock           cuvidCtxLock = nullptr;
    PFN_cuvidCtxUnlock         cuvidCtxUnlock = nullptr;
};

inline bool Nvdec_LoadNvdecApi(NvdecApi& a) {
    a.cudaDll = LoadLibrary(TEXT("nvcuda.dll"));
    a.cuvidDll = LoadLibrary(TEXT("nvcuvid.dll"));
    if (!a.cudaDll || !a.cuvidDll) return false;

#define RESOLVE_CUDA(field, name, alt) \
    a.field = (PFN_##field)GetProcAddress(a.cudaDll, name); \
    if (!a.field && (alt)) a.field = (PFN_##field)GetProcAddress(a.cudaDll, alt)
#define RESOLVE_CUVID(field, name) \
    a.field = (PFN_##field)GetProcAddress(a.cuvidDll, name)

    RESOLVE_CUDA(cuInit,              "cuInit",              nullptr);
    RESOLVE_CUDA(cuDeviceGetCount,   "cuDeviceGetCount",    nullptr);
    RESOLVE_CUDA(cuDeviceGet,        "cuDeviceGet",          nullptr);
    RESOLVE_CUDA(cuDeviceGetName,    "cuDeviceGetName",      nullptr);
    RESOLVE_CUDA(cuCtxCreate,        "cuCtxCreate_v2",      "cuCtxCreate");
    RESOLVE_CUDA(cuCtxDestroy,       "cuCtxDestroy_v2",      "cuCtxDestroy");
    RESOLVE_CUDA(cuCtxPushCurrent,  "cuCtxPushCurrent_v2", "cuCtxPushCurrent");
    RESOLVE_CUDA(cuCtxPopCurrent,   "cuCtxPopCurrent_v2",  "cuCtxPopCurrent");
    RESOLVE_CUDA(cuMemAlloc,         "cuMemAlloc_v2",        "cuMemAlloc");
    RESOLVE_CUDA(cuMemFree,          "cuMemFree_v2",         "cuMemFree");
    RESOLVE_CUDA(cuMemcpyDtoH,       "cuMemcpyDtoH_v2",      "cuMemcpyDtoH");
    RESOLVE_CUDA(cuStreamSynchronize,"cuStreamSynchronize", nullptr);
    // CUDA-D3D11 interop(零拷贝,可选 —— 解析不到则回退 CPU 路径)
    RESOLVE_CUDA(cuD3D11CtxCreate,                   "cuD3D11CtxCreate",                   nullptr);
    RESOLVE_CUDA(cuGraphicsD3D11RegisterResource,    "cuGraphicsD3D11RegisterResource",    nullptr);
    RESOLVE_CUDA(cuGraphicsUnregisterResource,       "cuGraphicsUnregisterResource",       nullptr);
    RESOLVE_CUDA(cuGraphicsMapResources,             "cuGraphicsMapResources",             nullptr);
    RESOLVE_CUDA(cuGraphicsUnmapResources,           "cuGraphicsUnmapResources",           nullptr);
    RESOLVE_CUDA(cuGraphicsSubResourceGetMappedArray,"cuGraphicsSubResourceGetMappedArray",nullptr);
    RESOLVE_CUDA(cuMemcpy2D,                         "cuMemcpy2D_v2",                      "cuMemcpy2D");

    RESOLVE_CUVID(cuvidCreateVideoParser,  "cuvidCreateVideoParser");
    RESOLVE_CUVID(cuvidParseVideoData,     "cuvidParseVideoData");
    RESOLVE_CUVID(cuvidDestroyVideoParser, "cuvidDestroyVideoParser");
    RESOLVE_CUVID(cuvidCreateDecoder,      "cuvidCreateDecoder");
    RESOLVE_CUVID(cuvidDestroyDecoder,     "cuvidDestroyDecoder");
    RESOLVE_CUVID(cuvidDecodePicture,      "cuvidDecodePicture");
    RESOLVE_CUVID(cuvidMapVideoFrame64,    "cuvidMapVideoFrame64");
    RESOLVE_CUVID(cuvidUnmapVideoFrame64,  "cuvidUnmapVideoFrame64");
    RESOLVE_CUVID(cuvidCtxLockCreate,      "cuvidCtxLockCreate");
    RESOLVE_CUVID(cuvidCtxLockDestroy,     "cuvidCtxLockDestroy");
    RESOLVE_CUVID(cuvidCtxLock,            "cuvidCtxLock");
    RESOLVE_CUVID(cuvidCtxUnlock,           "cuvidCtxUnlock");

#undef RESOLVE_CUDA
#undef RESOLVE_CUVID

    return a.cuInit && a.cuDeviceGetCount && a.cuDeviceGet && a.cuDeviceGetName &&
           a.cuCtxCreate && a.cuCtxDestroy && a.cuCtxPushCurrent && a.cuCtxPopCurrent &&
           a.cuMemAlloc && a.cuMemFree && a.cuMemcpyDtoH && a.cuStreamSynchronize &&
           a.cuvidCreateVideoParser && a.cuvidParseVideoData && a.cuvidDestroyVideoParser &&
           a.cuvidCreateDecoder && a.cuvidDestroyDecoder && a.cuvidDecodePicture &&
           a.cuvidMapVideoFrame64 && a.cuvidUnmapVideoFrame64 &&
           a.cuvidCtxLockCreate && a.cuvidCtxLockDestroy && a.cuvidCtxLock && a.cuvidCtxUnlock;
}