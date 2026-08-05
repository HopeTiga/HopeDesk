#pragma once

// Minimal media::DecoderBuffer / DecryptConfig for the AV1 DXVA port.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "chromiumShims/MediaUtils.h"

namespace media {

class DecryptConfig {
public:
    std::unique_ptr<DecryptConfig> Clone() const {
        return std::make_unique<DecryptConfig>();
    }
};

struct DecryptConfigSideData {
    uint64_t secure_handle = 0;
};

// Ref-counted byte buffer holding one AV1 access unit. Extends
// webrtc::RefCountInterface so it can be wrapped in webrtc::RefCountedObject.
class DecoderBuffer : public webrtc::RefCountInterface {
public:
    DecoderBuffer() = default;
    DecoderBuffer(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const DecryptConfig* decrypt_config() const { return decrypt_config_.get(); }
    void set_decrypt_config(std::unique_ptr<DecryptConfig> config) {
        decrypt_config_ = std::move(config);
    }
    const DecryptConfigSideData* side_data() const { return &side_data_; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::unique_ptr<DecryptConfig> decrypt_config_;
    DecryptConfigSideData side_data_;
};

} // namespace media
