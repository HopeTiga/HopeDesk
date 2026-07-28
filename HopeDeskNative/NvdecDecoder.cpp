#include "NvdecDecoder.h"
#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include <modules/video_coding/include/video_error_codes.h>
#include <third_party/libyuv/libyuv.h>

#include <cstring>
#include <cstdio>
#include "Utils.h"

namespace hope {
namespace rtc {

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------
NvdecDecoder::NvdecDecoder(Codec type)
    : codecType(type),
    cuvidCodec(type == Codec::H264 ? cudaVideoCodec_H264 :
                   type == Codec::H265 ? cudaVideoCodec_HEVC :
                   cudaVideoCodec_AV1) {
}

NvdecDecoder::~NvdecDecoder() {
    Release();
}

// ---------------------------------------------------------------------------
//  Configure —— 建立 CUDA 上下文 + CUVID parser
// ---------------------------------------------------------------------------
bool NvdecDecoder::Configure(const Settings& /*settings*/) {
    std::lock_guard<std::mutex> lock(mutex);
    if (videoParser) return true;          // 已配置过

    if (!EnsureContext()) {
        LOG_ERROR("[NvdecDecoder] EnsureContext failed (no NVIDIA driver / nvcuvid.dll?)");
        return false;
    }

    return RecreateParser();
}

// (重新)创建 CUVID parser(Configure / Flush 复用)
bool NvdecDecoder::RecreateParser() {
    if (videoParser) { nvdecApi.cuvidDestroyVideoParser(videoParser); videoParser = nullptr; }

    CUVIDPARSERPARAMS parserParams{};
    parserParams.CodecType = cuvidCodec;
    parserParams.ulMaxNumDecodeSurfaces = 16;
    parserParams.ulClockRate = 1000;            // 1kHz -> timestamp 即毫秒
    parserParams.ulErrorThreshold = 100;        // 即使有错也尝试解码
    parserParams.ulMaxDisplayDelay = 0;         // 低延迟,无重排序
    // AV1 是低开销 OBU -> bAnnexb=0;
    // H264/H265 来自 WebRTC 是 Annex-B(00 00 00 01 起始码) -> bAnnexb=1。
    // CUVID 不自动识别 annex-B,bAnnexb 设错则 parser 不出 OnVideoSequence、无帧、无画面。
    parserParams.bAnnexb = (codecType == Codec::AV1) ? 0 : 1;
    parserParams.pUserData = this;
    parserParams.pfnSequenceCallback = &NvdecDecoder::OnVideoSequence;
    parserParams.pfnDecodePicture = &NvdecDecoder::OnPictureDecode;
    parserParams.pfnDisplayPicture = &NvdecDecoder::OnPictureDisplay;
    parserParams.pExtVideoInfo = nullptr;

    CUresult result = nvdecApi.cuvidCreateVideoParser(&videoParser, &parserParams);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuvidCreateVideoParser failed: %d", result);
        videoParser = nullptr;
        return false;
    }

    const char* codecName = (codecType == Codec::H264) ? "H264" :
                                (codecType == Codec::H265) ? "H265" : "AV1";
    LOG_INFO("[NvdecDecoder] 硬件解码已配置成功 codec=%s", codecName);
    return true;
}

// ---------------------------------------------------------------------------
//  EnsureContext —— 加载 nvcuda/nvcuvid,选 NVIDIA 设备,建 CUDA 上下文 + ctxLock
// ---------------------------------------------------------------------------
bool NvdecDecoder::EnsureContext() {
    if (contextReady) return true;

    if (!Nvdec_LoadNvdecApi(nvdecApi)) {
        LOG_ERROR("[NvdecDecoder] LoadNvdecApi failed");
        return false;
    }

    CUresult result = nvdecApi.cuInit(0);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuInit failed: %d", result);
        return false;
    }

    int count = 0;
    nvdecApi.cuDeviceGetCount(&count);
    bool found = false;
    for (int i = 0; i < count; ++i) {
        CUdevice device = 0;
        if (nvdecApi.cuDeviceGet(&device, i) != CUDA_SUCCESS) continue;
        char name[256] = {};
        nvdecApi.cuDeviceGetName(name, sizeof(name), device);
        if (strstr(name, "NVIDIA")) { this->nvDevice = device; found = true; break; }
    }
    if (!found && count > 0) nvdecApi.cuDeviceGet(&this->nvDevice, 0);

    char gpuName[256] = {};
    nvdecApi.cuDeviceGetName(gpuName, sizeof(gpuName), this->nvDevice);
    LOG_INFO("[NvdecDecoder] CUDA device: %s", found ? gpuName : "(first device)");

    result = nvdecApi.cuCtxCreate(&cudaContext, 0, this->nvDevice);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuCtxCreate failed: %d", result);
        return false;
    }
    result = nvdecApi.cuvidCtxLockCreate(&contextLock, cudaContext);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuvidCtxLockCreate failed: %d", result);
        return false;
    }

    contextReady = true;
    return true;
}

// ---------------------------------------------------------------------------
//  ReinitDecoder —— 序列变化时(首次/分辨率变更)重建 CUVID decoder
// ---------------------------------------------------------------------------
bool NvdecDecoder::ReinitDecoder(const CUVIDEOFORMAT* format) {
    if (!format) return false;

    unsigned long codedWidth = format->coded_width;
    unsigned long codedHeight = format->coded_height;
    unsigned long displayWidth = (format->display_area.right > format->display_area.left)
                                     ? (unsigned long)(format->display_area.right - format->display_area.left) : codedWidth;
    unsigned long displayHeight = (format->display_area.bottom > format->display_area.top)
                                      ? (unsigned long)(format->display_area.bottom - format->display_area.top) : codedHeight;

    // 尺寸没变 -> 复用现有 decoder
    if (videoDecoder && this->codedWidth == (int)codedWidth && this->codedHeight == (int)codedHeight) return true;

    if (videoDecoder) {
        nvdecApi.cuvidDestroyDecoder(videoDecoder);
        videoDecoder = nullptr;
    }

    CUVIDDECODECREATEINFO createInfo{};
    createInfo.ulWidth = codedWidth;
    createInfo.ulHeight = codedHeight;
    createInfo.ulNumDecodeSurfaces = 16;
    createInfo.CodecType = cuvidCodec;
    createInfo.ChromaFormat = cudaVideoChromaFormat_420;
    createInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    createInfo.bitDepthMinus8 = format->bit_depth_luma_minus8;
    createInfo.ulIntraDecodeOnly = 0;
    createInfo.ulMaxWidth = codedWidth;
    createInfo.ulMaxHeight = codedHeight;
    createInfo.OutputFormat = cudaVideoSurfaceFormat_NV12;
    createInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    createInfo.ulTargetWidth = codedWidth;
    createInfo.ulTargetHeight = codedHeight;
    createInfo.ulNumOutputSurfaces = 2;
    createInfo.vidLock = contextLock;

    CUresult result = nvdecApi.cuvidCreateDecoder(&videoDecoder, &createInfo);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuvidCreateDecoder failed: %d", result);
        videoDecoder = nullptr;
        return false;
    }
    this->codedWidth = (int)codedWidth;
    this->codedHeight = (int)codedHeight;
    this->displayWidth = (int)displayWidth;
    this->displayHeight = (int)displayHeight;
    LOG_INFO("[NvdecDecoder] decoder created %lux%lu (display %lux%lu)", codedWidth, codedHeight, displayWidth, displayHeight);
    return true;
}

// ---------------------------------------------------------------------------
//  Flush —— parser/decoder 进入坏状态时重建(保留 CUDA context + ctxLock)
// ---------------------------------------------------------------------------
void NvdecDecoder::Flush() {
    if (videoDecoder) { nvdecApi.cuvidDestroyDecoder(videoDecoder); videoDecoder = nullptr; }
    codedWidth = codedHeight = 0;
    displayWidth = displayHeight = 0;
    pictureMeta.clear();
    RecreateParser();
    if (videoParser) LOG_INFO("[NvdecDecoder] parser/decoder flushed & recreated after decode error");
}

// ---------------------------------------------------------------------------
//  Decode —— 喂数据给 parser
// ---------------------------------------------------------------------------
int32_t NvdecDecoder::Decode(const webrtc::EncodedImage& inputImage,
                             bool missingFrames,
                             int64_t renderTimeMs) {

    std::lock_guard<std::mutex> lock(mutex);

    if (!videoParser) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

    pendingRtp = inputImage.RtpTimestamp();
    pendingRenderMs = renderTimeMs;

    CUVIDSOURCEDATAPACKET packet{};
    packet.payload = inputImage.data();
    packet.payload_size = (unsigned long)inputImage.size();
    packet.flags = CUVID_PKT_ENDOFPICTURE | CUVID_PKT_TIMESTAMP;
    if (missingFrames) packet.flags |= CUVID_PKT_DISCONTINUITY;
    packet.timestamp = (CUvideotimestamp)renderTimeMs;

    CUresult pushResult = nvdecApi.cuCtxPushCurrent(cudaContext);
    if (pushResult != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuCtxPushCurrent failed: %d", pushResult);
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    CUresult result = nvdecApi.cuvidParseVideoData(videoParser, &packet);

    CUcontext popped = nullptr;
    nvdecApi.cuCtxPopCurrent(&popped);

    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuvidParseVideoData failed: %d", result);
        Flush();
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

// ---------------------------------------------------------------------------
//  CUVID 回调
// ---------------------------------------------------------------------------
int CUDAAPI NvdecDecoder::OnVideoSequence(void* userData, CUVIDEOFORMAT* format) {
    auto* self = static_cast<NvdecDecoder*>(userData);
    if (!self || !format) return 0;

    if (!self->ReinitDecoder(format)) {
        LOG_ERROR("[NvdecDecoder] ReinitDecoder failed -> parser 将无法解码");
        return 0;
    }

    return 1;
}

int CUDAAPI NvdecDecoder::OnPictureDecode(void* userData, CUVIDPICPARAMS* pictureParams) {
    auto* self = static_cast<NvdecDecoder*>(userData);
    if (!self || !self->videoDecoder || !pictureParams) return 0;

    self->pictureMeta[pictureParams->CurrPicIdx] = { self->pendingRtp, self->pendingRenderMs, false };

    CUresult result = self->nvdecApi.cuvidDecodePicture(self->videoDecoder, pictureParams);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuvidDecodePicture failed: %d", result);
        return 0;
    }
    return 1;
}

int CUDAAPI NvdecDecoder::OnPictureDisplay(void* userData, CUVIDPARSERDISPINFO* displayInfo) {
    auto* self = static_cast<NvdecDecoder*>(userData);
    if (!self || !displayInfo) return 1;
    self->EmitFrame(displayInfo->picture_index, displayInfo->timestamp);
    return 1;
}

// ---------------------------------------------------------------------------
//  EmitFrame —— 取 NV12 设备帧 -> cuMemcpyDtoH 回主机 -> NV12Buffer -> 回调
// ---------------------------------------------------------------------------
void NvdecDecoder::EmitFrame(int pictureIndex, int64_t /*timestamp*/) {
    if (!videoDecoder || !decodeCallback) return;

    // 取回时间戳
    uint32_t rtp = 0;
    int64_t renderMs = 0;
    auto it = pictureMeta.find(pictureIndex);
    if (it != pictureMeta.end()) {
        rtp = it->second.rtp;
        renderMs = it->second.renderMs;
    }

    int width = displayWidth;     // 显示宽(输出尺寸)
    int height = displayHeight;   // 显示高
    int codedHeight = this->codedHeight;
    if (width <= 0 || height <= 0 || codedHeight <= 0) return;

    CUVIDPROCPARAMS procParams{};
    procParams.progressive_frame = 1;
    procParams.second_field = 0;
    procParams.top_field_first = 1;
    procParams.unpaired_field = 0;

    unsigned long long devicePtr = 0;
    unsigned int pitch = 0;
    CUresult result = nvdecApi.cuvidMapVideoFrame64(videoDecoder, pictureIndex, &devicePtr, &pitch, &procParams);
    if (result != CUDA_SUCCESS || devicePtr == 0 || pitch == 0) {
        LOG_ERROR("[NvdecDecoder] cuvidMapVideoFrame64 failed: %d", result);
        return;
    }

    // NV12: Y = pitch * codedHeight, UV = pitch * codedHeight / 2(用 coded 尺寸计算缓冲)
    size_t ySize = (size_t)pitch * (size_t)codedHeight;
    size_t totalSize = ySize + ySize / 2;
    if (hostNv12.size() < totalSize) hostNv12.resize(totalSize);

    result = nvdecApi.cuMemcpyDtoH(hostNv12.data(), (CUdeviceptr)devicePtr, totalSize);
    nvdecApi.cuvidUnmapVideoFrame64(videoDecoder, devicePtr);
    if (result != CUDA_SUCCESS) {
        LOG_ERROR("[NvdecDecoder] cuMemcpyDtoH failed: %d", result);
        return;
    }
    nvdecApi.cuStreamSynchronize((CUstream)0);

    const uint8_t* srcY = hostNv12.data();
    const uint8_t* srcUV = hostNv12.data() + ySize;
    auto nv12Buffer = webrtc::NV12Buffer::Create(width, height);
    uint8_t* dstY = nv12Buffer->MutableDataY();
    uint8_t* dstUV = nv12Buffer->MutableDataUV();
    int strideY = nv12Buffer->StrideY();
    int strideUV = nv12Buffer->StrideUV();
    int uvPitch = (int)pitch;

    // Y 平面:src 每行 pitch 字节 -> dst 每行 strideY 字节,每行拷 width 字节
    libyuv::CopyPlane(srcY, uvPitch, dstY, strideY, width, height);
    // UV 交错平面:每行 width 字节(width/2 个 UV 样本 x 2 字节),height/2 行
    libyuv::CopyPlane(srcUV, uvPitch, dstUV, strideUV, width, height / 2);

    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(nv12Buffer)
                                   .set_timestamp_rtp(rtp)
                                   .set_timestamp_ms(renderMs)
                                   .build();
    decodeCallback->Decoded(frame);

    pictureMeta.erase(pictureIndex);
}

// ---------------------------------------------------------------------------
//  RegisterDecodeCompleteCallback / Release / GetDecoderInfo
// ---------------------------------------------------------------------------
int32_t NvdecDecoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
    decodeCallback = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvdecDecoder::Release() {
    std::lock_guard<std::mutex> lock(mutex);
    if (videoParser) { nvdecApi.cuvidDestroyVideoParser(videoParser); videoParser = nullptr; }
    if (videoDecoder) { nvdecApi.cuvidDestroyDecoder(videoDecoder); videoDecoder = nullptr; }
    if (contextLock) { nvdecApi.cuvidCtxLockDestroy(contextLock); contextLock = nullptr; }
    if (cudaContext) { nvdecApi.cuCtxDestroy(cudaContext); cudaContext = nullptr; }
    if (nvdecApi.cuvidDll) { FreeLibrary(nvdecApi.cuvidDll); nvdecApi.cuvidDll = nullptr; }
    if (nvdecApi.cudaDll) { FreeLibrary(nvdecApi.cudaDll); nvdecApi.cudaDll = nullptr; }
    contextReady = false;
    codedWidth = codedHeight = 0;
    displayWidth = displayHeight = 0;
    hostNv12.clear();
    hostNv12.shrink_to_fit();
    pictureMeta.clear();
    return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo NvdecDecoder::GetDecoderInfo() const {
    DecoderInfo info;
    info.implementation_name = "NVDEC";
    info.is_hardware_accelerated = true;
    return info;
}

} // namespace rtc
} // namespace hope