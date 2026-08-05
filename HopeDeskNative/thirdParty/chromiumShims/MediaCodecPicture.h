#pragma once

// Minimal media::CodecPicture for the AV1 DXVA port.
#include <memory>
#include <optional>
#include <utility>

#include "chromiumShims/MediaUtils.h"
#include "chromiumShims/MediaDecoderBuffer.h"
#include "chromiumShims/MediaVideoTypes.h"
#include "chromiumShims/GfxGeometry.h"
#include "chromiumShims/GfxHdrMetadata.h"

namespace media {

// Extends webrtc::RefCountInterface so it can be wrapped in
// webrtc::RefCountedObject and managed with webrtc::scoped_refptr.
class CodecPicture : public webrtc::RefCountInterface {
public:
    CodecPicture() = default;

    void set_bitstream_id(int id) { bitstream_id_ = id; }
    int bitstream_id() const { return bitstream_id_; }

    void set_visible_rect(const gfx::Rect& rect) { visible_rect_ = rect; }
    const gfx::Rect& visible_rect() const { return visible_rect_; }

    void set_colorspace(const VideoColorSpace& cs) { color_space_ = cs; }
    const VideoColorSpace& get_colorspace() const { return color_space_; }

    void set_hdr_metadata(const std::optional<gfx::HDRMetadata>& metadata) {
        hdr_metadata_ = metadata;
    }
    const std::optional<gfx::HDRMetadata>& hdr_metadata() const { return hdr_metadata_; }

    void set_decrypt_config(std::unique_ptr<DecryptConfig> config) {
        decrypt_config_ = std::move(config);
    }
    const DecryptConfig* decrypt_config() const { return decrypt_config_.get(); }

protected:
    ~CodecPicture() override = default;

private:
    int bitstream_id_ = 0;
    gfx::Rect visible_rect_;
    VideoColorSpace color_space_;
    std::optional<gfx::HDRMetadata> hdr_metadata_;
    std::unique_ptr<DecryptConfig> decrypt_config_;
};

} // namespace media
