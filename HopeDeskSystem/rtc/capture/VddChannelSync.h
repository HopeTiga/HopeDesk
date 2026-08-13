#pragma once

#include <atomic>
#include <cstdint>

namespace hope {
namespace rtc {

// 采集线程与 NVENC 编码器共享的通道同步状态。
struct VddChannelSync {
    std::atomic<uint32_t> generation{0};       // 重开通道 +1，编码器据此清 handle 缓存
    std::atomic<uint32_t> reopenRequested{0};  // 编码器 AcquireSync 失败置 1，采集线程重开
};

} // namespace rtc
} // namespace hope
