#pragma once

#include <atomic>
#include <cstdint>

namespace hope {
namespace rtc {

// VDD 共享帧通道的同步状态，由上游 VirtualDisplayCapture 与下游 NVENC
// 编码器共享（经 WebrtcVideoEncoderFactory 注入）。
//
// 背景：驱动在显示变化时（swap-chain 重建、拓扑/分辨率切换）会重建共享
// 纹理并 bump ChannelGeneration。编码器手里还是旧纹理的 keyed mutex，
// 驱动不会再在它上面 ReleaseSync(1)，导致 AcquireSync 超时/挂死。
//
// 握手协议（双向通知，全部原子操作，可在编码线程/捕获线程之间安全传递）：
//   * 下游（编码器）在 keyed-mutex AcquireSync 失败时置 reopenRequested = 1，
//     请求上游重开通道。
//   * 上游（捕获线程）消费该标志：closeFrameChannel + openFrameChannel，
//     成功后 generation++。
//   * 下游在每次 Encode 时发现 generation 变化，即清空按 handle 缓存的
//     resourceCache —— 旧的 keyed-mutex 已全部失效，必须用新 handle 重新打开。
struct VddChannelSync {
    std::atomic<uint32_t> generation{0};       // 上游每重开一次 +1
    std::atomic<uint32_t> reopenRequested{0};  // 下游置 1，上游消费
};

} // namespace rtc
} // namespace hope
