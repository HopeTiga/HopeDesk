#pragma once

// Minimal SVCGenericMetadata for the AV1 DXVA port (from
// media/video/video_encode_accelerator.h). Unused in decode.
namespace media {

struct SVCGenericMetadata {
    bool has_generic_metadata = false;
};

} // namespace media
