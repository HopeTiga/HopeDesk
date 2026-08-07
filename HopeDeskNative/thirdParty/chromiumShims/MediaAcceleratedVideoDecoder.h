#pragma once

// Minimal media::AcceleratedVideoDecoder base for the AV1 DXVA port.
#include <cstddef>
#include <cstdint>
#include <optional>

#include "chromiumShims/MediaUtils.h"
#include "chromiumShims/MediaDecoderBuffer.h"
#include "chromiumShims/MediaVideoTypes.h"
#include "chromiumShims/GfxGeometry.h"
#include "chromiumShims/GfxHdrMetadata.h"

namespace media {

class AcceleratedVideoDecoder {
public:
    enum DecodeResult {
        kRanOutOfStreamData = 0,
        kRanOutOfSurfaces = 1,
        kNeedContextRestart = 2,
        kDecodeError = 3,
        kTryAgain = 4,
        kConfigChange = 5,
    };

    virtual void SetStream(int32_t id,
                           webrtc::scoped_refptr<DecoderBuffer> decoder_buffer) = 0;
    virtual bool Flush() = 0;
    virtual void Reset() = 0;
    virtual DecodeResult Decode() = 0;
    virtual gfx::Size GetPicSize() const = 0;
    virtual gfx::Rect GetVisibleRect() const = 0;
    virtual VideoCodecProfile GetProfile() const = 0;
    virtual uint8_t GetBitDepth() const = 0;
    virtual VideoChromaSampling GetChromaSampling() const = 0;
    virtual VideoColorSpace GetVideoColorSpace() const = 0;
    virtual std::optional<gfx::HDRMetadata> GetHDRMetadata() const = 0;
    virtual size_t GetRequiredNumOfPictures() const = 0;
    virtual size_t GetNumReferenceFrames() const = 0;

    virtual ~AcceleratedVideoDecoder() = default;
};

} // namespace media
