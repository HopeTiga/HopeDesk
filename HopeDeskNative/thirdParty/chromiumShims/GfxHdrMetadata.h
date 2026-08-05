#pragma once

// Minimal gfx HDR metadata shims for the AV1 DXVA port. Only the containers
// are kept; the renderer does not consume HDR metadata.
#include <array>
#include <cstdint>
#include <optional>

#include "chromiumShims/BaseSpan.h"

namespace gfx {

struct HdrMetadataSmpteSt2086 {
    HdrMetadataSmpteSt2086() = default;
    HdrMetadataSmpteSt2086(std::array<float, 8> primaries_in,
                           float luminance_max_in,
                           float luminance_min_in)
        : primaries(primaries_in),
          luminance_max(luminance_max_in),
          luminance_min(luminance_min_in) {}

    std::array<float, 8> primaries{};
    float luminance_max = 0.0f;
    float luminance_min = 0.0f;
};

struct HdrMetadataCta861_3 {
    HdrMetadataCta861_3() = default;
    HdrMetadataCta861_3(int max_cll_in, int max_fall_in)
        : max_cll(max_cll_in), max_fall(max_fall_in) {}

    int max_cll = 0;
    int max_fall = 0;
};

struct HdrMetadataAgtm {};

struct HDRMetadata {
    HdrMetadataSmpteSt2086 smpte_st_2086;
    HdrMetadataCta861_3 cta_861_3;
    std::optional<HdrMetadataAgtm> agtm;
};

// AGTM (advanced GPU tone mapping) metadata is not parsed in this port.
inline std::optional<HdrMetadataAgtm> GetHdrMetadataAgtmFromItutT35(
    uint8_t /*country_code*/, base::span<const uint8_t> /*payload*/) {
    return std::nullopt;
}

} // namespace gfx
