#pragma once

// Minimal absl::Cleanup for the AV1 DXVA port.
#include <utility>

namespace absl {

template <typename F>
class Cleanup {
public:
    Cleanup(F f) : f_(std::move(f)), cancelled_(false) {}
    Cleanup(Cleanup&& o) noexcept
        : f_(std::move(o.f_)), cancelled_(o.cancelled_) {
        o.cancelled_ = true;
    }
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;

    ~Cleanup() {
        if (!cancelled_)
            f_();
    }

    void Cancel() { cancelled_ = true; }

private:
    F f_;
    bool cancelled_;
};

} // namespace absl
