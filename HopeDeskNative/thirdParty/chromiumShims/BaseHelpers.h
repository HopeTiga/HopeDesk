#pragma once

// Minimal base helpers for the AV1 DXVA port.
#include <memory>
#include <utility>

#include "chromiumShims/MediaUtils.h"

namespace base {

template <typename T, typename U>
inline T checked_cast(U value) {
    return static_cast<T>(value);
}

template <typename T, typename U>
inline T strict_cast(U value) {
    return static_cast<T>(value);
}

template <typename T>
inline std::unique_ptr<T> WrapUnique(T* ptr) {
    return std::unique_ptr<T>(ptr);
}

template <typename T, typename... Args>
inline std::unique_ptr<T> WrapUniqueNew(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// FeatureList / feature flags are never enabled in this port.
struct Feature {};
class FeatureList {
public:
    static bool IsEnabled(const Feature&) { return false; }
};

// base::MakeRefCounted (base/memory/scoped_refptr.h). Returns a webrtc::scoped_refptr.
// T must derive from webrtc::RefCountInterface (it is wrapped in RefCountedObject).
template <typename T, typename... Args>
inline auto MakeRefCounted(Args&&... args) {
    return webrtc::scoped_refptr<T>(
        new webrtc::RefCountedObject<T>(std::forward<Args>(args)...));
}

} // namespace base
