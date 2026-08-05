#pragma once

// base::raw_ptr / SEQUENCE_CHECKER shims for the AV1 DXVA port.
#include <cstddef>

#define SEQUENCE_CHECKER(seq)
#define DCHECK_CALLED_ON_VALID_SEQUENCE(seq) ((void)0)

namespace base {

template <typename T>
class raw_ptr {
public:
    raw_ptr() = default;
    raw_ptr(T* p) : ptr_(p) {}
    raw_ptr(std::nullptr_t) : ptr_(nullptr) {}
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    raw_ptr& operator=(T* p) { ptr_ = p; return *this; }
    raw_ptr& operator=(std::nullptr_t) { ptr_ = nullptr; return *this; }

private:
    T* ptr_ = nullptr;
};

} // namespace base
