#pragma once

// Minimal base::span shim for the AV1 DXVA port. Only supports the operations
// used by media/gpu/av1_decoder.cc and d3d11_av1_accelerator.cc.
#include <cstddef>

namespace base {

template <typename T>
class span {
public:
    constexpr span() noexcept : data_(nullptr), size_(0) {}
    constexpr span(T* ptr, size_t count) noexcept : data_(ptr), size_(count) {}
    span(T* first, T* last) noexcept : data_(first), size_(last - first) {}

    constexpr T* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    T& operator[](size_t i) const noexcept { return data_[i]; }
    constexpr T* begin() const noexcept { return data_; }
    constexpr T* end() const noexcept { return data_ + size_; }

private:
    T* data_;
    size_t size_;
};

} // namespace base
