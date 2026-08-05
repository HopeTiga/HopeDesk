#pragma once

// Minimal media video type shims (VideoCodecProfile / VideoChromaSampling /
// VideoColorSpace) for the AV1 DXVA port.
#include <cstdint>
#include <string>

namespace gfx {
namespace ColorSpace {
enum RangeID {
    INVALID = -1,
    LIMITED = 0,
    FULL = 1,
};
} // namespace ColorSpace
} // namespace gfx

namespace media {

enum VideoCodecProfile {
    VIDEO_CODEC_PROFILE_UNKNOWN = -1,
    AV1PROFILE_PROFILE_MAIN = 0,
    AV1PROFILE_PROFILE_HIGH = 1,
    AV1PROFILE_PROFILE_PRO = 2,
};

inline const char* GetProfileName(VideoCodecProfile profile) {
    switch (profile) {
        case AV1PROFILE_PROFILE_MAIN: return "AV1 Main";
        case AV1PROFILE_PROFILE_HIGH: return "AV1 High";
        case AV1PROFILE_PROFILE_PRO:  return "AV1 Pro";
        default: return "Unknown";
    }
}

enum class VideoChromaSampling {
    kUnknown,
    k400,
    k420,
    k422,
    k444,
};

// Color space is only stored/compared here; the renderer does not use it.
class VideoColorSpace {
public:
    VideoColorSpace() = default;
    VideoColorSpace(int primaries, int transfer, int matrix, int range)
        : primaries_(primaries), transfer_(transfer), matrix_(matrix), range_(range) {}

    bool IsSpecified() const { return primaries_ >= 0; }
    bool operator==(const VideoColorSpace& o) const {
        return primaries_ == o.primaries_ && transfer_ == o.transfer_ &&
               matrix_ == o.matrix_ && range_ == o.range_;
    }
    bool operator!=(const VideoColorSpace& o) const { return !(*this == o); }
    std::string ToString() const { return std::string(); }

private:
    int primaries_ = -1;
    int transfer_ = -1;
    int matrix_ = -1;
    int range_ = -1;
};

} // namespace media
