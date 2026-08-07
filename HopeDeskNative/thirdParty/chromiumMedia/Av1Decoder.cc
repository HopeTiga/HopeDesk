// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Av1Decoder.h"

#include <bitset>
#include <cstdlib>
#include <utility>

#include "chromiumShims/BaseHelpers.h"
#include "chromiumShims/MediaLimits.h"
#include "chromiumShims/MediaSwitches.h"
#include "chromiumShims/AbslCleanup.h"
#include "Utils.h"
#include "Av1Picture.h"
#include "libgav1/decoder_state.h"
#include "libgav1/gav1/status_code.h"
#include "libgav1/utils/common.h"
#include "libgav1/utils/constants.h"
#include "chromiumShims/GfxHdrMetadata.h"

namespace media {
namespace {
// (Section 6.4.1):
//
// - "An operating point specifies which spatial and temporal layers should be
//   decoded."
//
// - "The order of operating points indicates the preferred order for producing
//   an output: a decoder should select the earliest operating point in the list
//   that meets its decoding capabilities as expressed by the level associated
//   with each operating point."
//
// For simplicity, we always select operating point 0 and will validate that it
// doesn't have scalability information.
constexpr unsigned int kDefaultOperatingPoint = 0;

// Conversion function from libgav1 profiles to media::VideoCodecProfile.
VideoCodecProfile AV1ProfileToVideoCodecProfile(
    libgav1::BitstreamProfile profile) {
  switch (profile) {
    case libgav1::kProfile0:
      return AV1PROFILE_PROFILE_MAIN;
    case libgav1::kProfile1:
      return AV1PROFILE_PROFILE_HIGH;
    case libgav1::kProfile2:
      return AV1PROFILE_PROFILE_PRO;
    default:
      // ObuParser::ParseSequenceHeader() validates the profile.
      abort();
  }
}

// Returns true iff the current decode sequence has multiple spatial layers.
bool IsSpatialLayerDecoding(int operatingPointIdc) {
  // Spec 6.4.1.
  constexpr int kTemporalLayerBitMaskBits = 8;
  const int kUsedSpatialLayerBitMask =
      (operatingPointIdc >> kTemporalLayerBitMaskBits) & 0b1111;
  // In case of an only temporal layer encoding e.g. L1T3, spatial layer#0 bit
  // is 1. We allow this case.
  return kUsedSpatialLayerBitMask > 1;
}

bool IsValidBitDepth(uint8_t bit_depth, VideoCodecProfile profile) {
  // Spec 6.4.1.
  switch (profile) {
    case AV1PROFILE_PROFILE_MAIN:
    case AV1PROFILE_PROFILE_HIGH:
      return bit_depth == 8u || bit_depth == 10u;
    case AV1PROFILE_PROFILE_PRO:
      return bit_depth == 8u || bit_depth == 10u || bit_depth == 12u;
    default:
      abort();
  }
}

VideoChromaSampling GetAV1ChromaSampling(
    const libgav1::ColorConfig& colorConfig) {
  // Spec section 6.4.2
  int8_t subsampling_x = colorConfig.subsampling_x;
  int8_t subsampling_y = colorConfig.subsampling_y;
  bool monochrome = colorConfig.is_monochrome;
  if (monochrome) {
    return VideoChromaSampling::k400;
  } else {
    if (subsampling_x == 0 && subsampling_y == 0) {
      return VideoChromaSampling::k444;
    } else if (subsampling_x == 1u && subsampling_y == 0) {
      return VideoChromaSampling::k422;
    } else if (subsampling_x == 1u && subsampling_y == 1u) {
      return VideoChromaSampling::k420;
    } else {
      LOG_WARN("Unknown chroma sampling format.");
      return VideoChromaSampling::kUnknown;
    }
  }
}

gfx::HdrMetadataSmpteSt2086 ToGfxSmpteSt2086(
    const libgav1::ObuMetadataHdrMdcv& mdcv) {
  constexpr auto kChromaDenominator = 65536.0f;
  constexpr auto kLumaMaxDenoninator = 256.0f;
  constexpr auto kLumaMinDenoninator = 16384.0f;
  // display primaries are in R/G/B order in metadata_hdr_mdcv OBU Metadata.
  return gfx::HdrMetadataSmpteSt2086(
      {mdcv.primary_chromaticity_x[0] / kChromaDenominator,
       mdcv.primary_chromaticity_y[0] / kChromaDenominator,
       mdcv.primary_chromaticity_x[1] / kChromaDenominator,
       mdcv.primary_chromaticity_y[1] / kChromaDenominator,
       mdcv.primary_chromaticity_x[2] / kChromaDenominator,
       mdcv.primary_chromaticity_y[2] / kChromaDenominator,
       mdcv.white_point_chromaticity_x / kChromaDenominator,
       mdcv.white_point_chromaticity_y / kChromaDenominator},
      /*luminance_max=*/mdcv.luminance_max / kLumaMaxDenoninator,
      /*luminance_min=*/mdcv.luminance_min / kLumaMinDenoninator);
}

gfx::HdrMetadataCta861_3 ToGfxCta861_3(const libgav1::ObuMetadataHdrCll& cll) {
  return gfx::HdrMetadataCta861_3(cll.max_cll, cll.max_fall);
}
}  // namespace

webrtc::scoped_refptr<AV1Picture> AV1Decoder::AV1Accelerator::createAV1PictureSecure(
    bool apply_grain,
    uint64_t secure_handle) {
  return nullptr;
}

AV1Decoder::AV1Accelerator::Status AV1Decoder::AV1Accelerator::setStream(
    base::span<const uint8_t> stream,
    const DecryptConfig* decryptConfig) {
  return Status::kOk;
}

AV1Decoder::AV1Decoder(std::unique_ptr<AV1Accelerator> accelerator,
                       VideoCodecProfile profile,
                       const VideoColorSpace& containerColorSpace)
    : bufferPool(std::make_unique<libgav1::BufferPool>(
          /*on_frame_buffer_size_changed=*/nullptr,
          /*get_frame_buffer=*/nullptr,
          /*release_frame_buffer=*/nullptr,
          /*callback_private_data=*/nullptr)),
      state(std::make_unique<libgav1::DecoderState>()),
      accelerator(std::move(accelerator)),
      profile(profile),
      containerColorSpace(containerColorSpace) {
  refFrames.fill(nullptr);
}

AV1Decoder::~AV1Decoder() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // |bufferPool| checks that all the allocated frames are released in its
  // dtor. Explicitly destruct |state| before |bufferPool| to release frames
  // in |reference_frame| in |state|.
  state.reset();
}

bool AV1Decoder::Flush() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG_DEBUG("AV1 decoder flush");
  Reset();
  return true;
}

void AV1Decoder::Reset() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  clearCurrentFrame();

  // We must reset the |currentSequenceHeader| to ensure we don't try to
  // decode frames using an incorrect sequence header. If the first
  // DecoderBuffer after the reset doesn't contain a sequence header, we'll just
  // skip it and will keep skipping until we get a sequence header.
  currentSequenceHeader.reset();
  streamId = 0;
  onError = false;

  state = std::make_unique<libgav1::DecoderState>();
  clearReferenceFrames();
  parser.reset();
  decryptConfig.reset();
  decoderBuffer = nullptr;
  secureHandle = 0;

  bufferPool = std::make_unique<libgav1::BufferPool>(
      /*on_frame_buffer_size_changed=*/nullptr,
      /*get_frame_buffer=*/nullptr,
      /*release_frame_buffer=*/nullptr,
      /*callback_private_data=*/nullptr);
}

void AV1Decoder::SetStream(int32_t id,
                           webrtc::scoped_refptr<DecoderBuffer> buffer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  decoderBuffer = std::move(buffer);
  streamId = id;
  clearCurrentFrame();

  parser = base::WrapUnique(new (std::nothrow) libgav1::ObuParser(
      decoderBuffer->data(), decoderBuffer->size(),
      kDefaultOperatingPoint, bufferPool.get(), state.get()));
  if (!parser) {
    onError = true;
    return;
  }

  if (currentSequenceHeader)
    parser->set_sequence_header(*currentSequenceHeader);
  if (decoderBuffer->decrypt_config()) {
    decryptConfig = decoderBuffer->decrypt_config()->Clone();
  } else {
    decryptConfig.reset();
  }
  if (decoderBuffer->side_data() &&
      decoderBuffer->side_data()->secure_handle) {
    secureHandle = decoderBuffer->side_data()->secure_handle;
  } else {
    secureHandle = 0;
  }

  const AV1Accelerator::Status status =
      accelerator->setStream(base::span<const uint8_t>(decoderBuffer->data(),
                                                        decoderBuffer->size()),
                              decryptConfig.get());
  if (status != AV1Accelerator::Status::kOk) {
    onError = true;
    return;
  }
}

void AV1Decoder::clearCurrentFrame() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  currentFrame.reset();
  currentFrameHeader.reset();
  pendingPic = nullptr;
}

AcceleratedVideoDecoder::DecodeResult AV1Decoder::Decode() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (onError)
    return kDecodeError;
  auto result = decodeInternal();
  onError = result == kDecodeError;
  return result;
}

AcceleratedVideoDecoder::DecodeResult AV1Decoder::decodeInternal() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!parser) {
    LOG_WARN("Decode() is called before SetStream()");
    return kRanOutOfStreamData;
  }
  while (parser->HasData() || currentFrameHeader) {
    absl::Cleanup clear_current_frame = [this] { clearCurrentFrame(); };
    if (pendingPic) {
      const AV1Accelerator::Status status = decodeAndOutputPicture(
          std::move(pendingPic), parser->tile_buffers());
      if (status == AV1Accelerator::Status::kFail)
        return kDecodeError;
      if (status == AV1Accelerator::Status::kTryAgain) {
        std::move(clear_current_frame).Cancel();
        return kTryAgain;
      }
      // Continue so that we force |clear_current_frame| to run before moving
      // on.
      continue;
    }
    if (!currentFrameHeader) {
      libgav1::StatusCode status_code = parser->ParseOneFrame(&currentFrame);
      if (status_code != libgav1::kStatusOk) {
        LOG_ERROR("[AV1Decoder] Failed to parse OBU: %s",
                  libgav1::GetErrorString(status_code));
        return kDecodeError;
      }
      if (!currentFrame) {
        LOG_WARN("No frame found. Skipping the current stream");
        continue;
      }

      currentFrameHeader = parser->frame_header();
      // Detects if a new coded video sequence is starting.
      if (parser->sequence_header_changed()) {
        if (IsSpatialLayerDecoding(
                parser->sequence_header()
                    .operating_point_idc[kDefaultOperatingPoint])) {
          LOG_ERROR("[AV1Decoder] Spatial layer decoding is not supported");
          return kDecodeError;
        }

        currentSequenceHeader = parser->sequence_header();
        VideoChromaSampling newChromaSampling =
            GetAV1ChromaSampling(currentSequenceHeader->color_config);
        if (newChromaSampling != chromaSampling) {
          chromaSampling = newChromaSampling;
        }

        if (chromaSampling != VideoChromaSampling::k420 &&
            chromaSampling != VideoChromaSampling::k444) {
          LOG_ERROR("[AV1Decoder] Only YUV 4:2:0 and YUV 4:4:4 are supported (chroma=%d)",
                    static_cast<int>(chromaSampling));
          return kDecodeError;
        }

        const VideoCodecProfile newProfile =
            AV1ProfileToVideoCodecProfile(currentSequenceHeader->profile);
        const uint8_t newBitDepth = base::checked_cast<uint8_t>(
            currentSequenceHeader->color_config.bitdepth);
        if (!IsValidBitDepth(newBitDepth, newProfile)) {
          LOG_ERROR("[AV1Decoder] Invalid bit depth=%d, profile=%s",
                    base::strict_cast<int>(newBitDepth),
                    GetProfileName(newProfile));
          return kDecodeError;
        }

        const gfx::Size newFrameSize(
            base::strict_cast<int>(currentSequenceHeader->max_frame_width),
            base::strict_cast<int>(currentSequenceHeader->max_frame_height));
        gfx::Rect newVisibleRect(
            base::strict_cast<int>(currentFrameHeader->width),
            base::strict_cast<int>(currentFrameHeader->height));
        if (!gfx::Rect(newFrameSize).Contains(newVisibleRect)) {
          LOG_DEBUG("Render size exceeds picture size. render size: %s, picture size: %s",
                    newVisibleRect.ToString().c_str(), newFrameSize.ToString().c_str());
          newVisibleRect = gfx::Rect(newFrameSize);
        }

        const auto& cc = currentSequenceHeader->color_config;
        const VideoColorSpace headerColorSpace =
            VideoColorSpace(cc.color_primary, cc.transfer_characteristics,
                            cc.matrix_coefficients,
                            cc.color_range == libgav1::kColorRangeStudio
                                ? gfx::ColorSpace::RangeID::LIMITED
                                : gfx::ColorSpace::RangeID::FULL);

        VideoColorSpace newColorSpace;
        // For AV1, prefer the frame color space over the config.
        if (headerColorSpace.IsSpecified()) {
          newColorSpace = headerColorSpace;
        } else if (containerColorSpace.IsSpecified()) {
          newColorSpace = containerColorSpace;
        }

        bool isColorSpaceChange = false;
        if (base::FeatureList::IsEnabled(kAVDColorSpaceChanges)) {
          isColorSpaceChange = newColorSpace.IsSpecified() &&
                                  newColorSpace != pictureColorSpace;
        }

        clearReferenceFrames();
        // Issues kConfigChange only if either the dimensions, profile or bit
        // depth is changed.
        if (frameSize != newFrameSize ||
            visibleRect != newVisibleRect || profile != newProfile ||
            bitDepth != newBitDepth || isColorSpaceChange) {
          LOG_DEBUG("New profile: %s, new resolution: %s, new visible rect: %s, new bit depth: %d, new color space: %s",
                    GetProfileName(newProfile), newFrameSize.ToString().c_str(),
                    newVisibleRect.ToString().c_str(), base::strict_cast<int>(newBitDepth),
                    newColorSpace.ToString().c_str());
          frameSize = newFrameSize;
          visibleRect = newVisibleRect;
          profile = newProfile;
          bitDepth = newBitDepth;
          pictureColorSpace = newColorSpace;
          std::move(clear_current_frame).Cancel();
          return kConfigChange;
        }
      }
    }

    if (!currentSequenceHeader) {
      // Decoding is not doable because we haven't received a sequence header.
      // This occurs when seeking a video.
      LOG_DEBUG("Discarded the current frame because no sequence header has been found yet");
      continue;
    }

    const auto& frameHeader = *currentFrameHeader;
    if (frameHeader.show_existing_frame) {
      const size_t frameToShow =
          base::checked_cast<size_t>(frameHeader.frame_to_show);
      if (!checkAndCleanUpReferenceFrames()) {
        LOG_ERROR("[AV1Decoder] show_existing_frame: ref frames differ from state");
        return kDecodeError;
      }

      auto pic = refFrames[frameToShow];
      pic = pic->duplicate();
      if (!pic) {
        LOG_ERROR("[AV1Decoder] show_existing_frame: Failed duplication");
        return kDecodeError;
      }

      pic->set_bitstream_id(streamId);
      if (!accelerator->outputPicture(*pic)) {
        LOG_ERROR("[AV1Decoder] show_existing_frame: outputPicture failed");
        return kDecodeError;
      }

      // libgav1::ObuParser sets |currentFrame| to the frame to show while
      // |currentFrameHeader| is the frame header of the currently parsed
      // frame. If |currentFrame| is a keyframe, then refresh_frame_flags must
      // be 0xff. Otherwise, refresh_frame_flags must be 0x00 (Section 5.9.2).
      updateReferenceFrames(std::move(pic));
      continue;
    }

    if (parser->tile_buffers().empty()) {
      // The last call to ParseOneFrame() didn't actually have any tile groups.
      // This could happen in rare cases (for example, if there is a Metadata
      // OBU after the TileGroup OBU). Ignore this case.
      continue;
    }

    const gfx::Size currentFrameSize(
        base::strict_cast<int>(frameHeader.width),
        base::strict_cast<int>(frameHeader.height));
    // As per the AV1 spec input video frames can be encoded at a lower
    // resolution and then the decoder reconstructs the frames back at the
    // scaled resolution. This is called as reference frame scaling.
    // In our case the scaled resolution is the one which is specified by
    // the sequence header.
    // https://gitlab.com/AOMediaCodec/SVT-AV1/-/blob/master/Docs/Appendix-Reference-Scaling.md
    if (currentFrameSize != frameSize) {
      LOG_DEBUG("Resolution change in the middle of video sequence. Frames encoded using reference frame scaling.");
    }
    if (currentFrameSize.width() !=
        base::strict_cast<int>(frameHeader.upscaled_width)) {
      LOG_ERROR("[AV1Decoder] Super resolution is not supported");
      return kDecodeError;
    }

    // As per the comments in third_party/libgav1/src/src/utils/types.h
    // for the ObuFrameHeader structure, the render_width and
    // render_height are hints to the application about the desired display
    // size. It has no effect on the decoding process. The visible rect should
    // be set to the current frames width and height.
    const gfx::Rect current_visible_rect(
        base::strict_cast<int>(frameHeader.width),
        base::strict_cast<int>(frameHeader.height));
    if (current_visible_rect != visibleRect) {
      LOG_DEBUG("Visible rectangle change in the middle of video sequence.");
      visibleRect = current_visible_rect;
    }

    // AV1 HDR metadata may appears in the below places:
    // 1. Container.
    // 2. Bitstream.
    // 3. Both container and bitstream.
    // Thus we should also extract HDR metadata here in case we
    // miss the information.
    if (currentFrame->hdr_cll_set()) {
      if (!hdrMetadata.has_value()) {
        hdrMetadata.emplace();
      }
      hdrMetadata->cta_861_3 = ToGfxCta861_3(currentFrame->hdr_cll());
    }
    if (currentFrame->hdr_mdcv_set()) {
      if (!hdrMetadata.has_value()) {
        hdrMetadata.emplace();
      }
      hdrMetadata->smpte_st_2086 =
          ToGfxSmpteSt2086(currentFrame->hdr_mdcv());
    }
    if (currentFrame->itut_t35_set()) {
      // SAFETY: The best we can do is trust the size provided by libgav1.
      auto t35_payload_span = base::span<const uint8_t>(
          currentFrame->itut_t35().payload_bytes,
          static_cast<size_t>(currentFrame->itut_t35().payload_size));
      const std::optional<gfx::HdrMetadataAgtm> agtm =
          gfx::GetHdrMetadataAgtmFromItutT35(currentFrame->itut_t35().country_code,
                                             t35_payload_span);
      if (agtm.has_value()) {
        if (!hdrMetadata.has_value()) {
          hdrMetadata.emplace();
        }
        // Overwrite existing AGTM metadata if any.
        hdrMetadata->agtm = agtm;
      }
    }

    auto pic = secureHandle ? accelerator->createAV1PictureSecure(
                                    frameHeader.film_grain_params.apply_grain,
                                    secureHandle)
                              : accelerator->createAV1Picture(
                                    frameHeader.film_grain_params.apply_grain);
    if (!pic) {
      std::move(clear_current_frame).Cancel();
      return kRanOutOfSurfaces;
    }

    pic->set_visible_rect(current_visible_rect);
    pic->set_bitstream_id(streamId);

    // Set the color space for the picture.
    pic->set_colorspace(pictureColorSpace);

    if (hdrMetadata)
      pic->set_hdr_metadata(hdrMetadata);

    pic->frameHeader = frameHeader;
    if (decryptConfig)
      pic->set_decrypt_config(decryptConfig->Clone());
    const AV1Accelerator::Status status =
        decodeAndOutputPicture(std::move(pic), parser->tile_buffers());
    if (status == AV1Accelerator::Status::kFail)
      return kDecodeError;
    if (status == AV1Accelerator::Status::kTryAgain) {
      std::move(clear_current_frame).Cancel();
      return kTryAgain;
    }
  }
  return kRanOutOfStreamData;
}

void AV1Decoder::updateReferenceFrames(webrtc::scoped_refptr<AV1Picture> pic) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint8_t refreshFrameFlags =
      currentFrameHeader->refresh_frame_flags;
  const std::bitset<libgav1::kNumReferenceFrameTypes> updateReferenceFrame(
      refreshFrameFlags);
  for (size_t i = 0; i < libgav1::kNumReferenceFrameTypes; ++i) {
    if (updateReferenceFrame[i])
      refFrames[i] = pic;
  }
  state->UpdateReferenceFrames(currentFrame,
                                base::strict_cast<int>(refreshFrameFlags));
}

void AV1Decoder::clearReferenceFrames() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  refFrames.fill(nullptr);
  // If AV1Decoder has decided to clear the reference frames, then ObuParser
  // must have also decided to do so.
}

bool AV1Decoder::checkAndCleanUpReferenceFrames() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (size_t i = 0; i < libgav1::kNumReferenceFrameTypes; ++i) {
    if (state->reference_frame[i] && !refFrames[i])
      return false;
    if (!state->reference_frame[i] && refFrames[i])
      refFrames[i] = nullptr;
  }

  // If we get here, we know |refFrames| includes all and only those frames
  // that can be currently used as reference frames. Now we'll assert that for
  // non-intra frames, all the necessary reference frames are in |refFrames|.
  // For intra frames, we don't need this assertion because they shouldn't
  // depend on reference frames.
  if (!libgav1::IsIntraFrame(currentFrameHeader->frame_type)) {
    for (int8_t ref_frame_index :
         currentFrameHeader->reference_frame_index) {
      // Unless an error occurred in libgav1, |ref_frame_index| should be valid,
      // and since checkAndCleanUpReferenceFrames() only gets called if parsing
      // succeeded, we can assert that validity.
    }
  }

  // If we get here, we know that all the reference frames needed by the current
  // frame are in |refFrames|.
  return true;
}

AV1Decoder::AV1Accelerator::Status AV1Decoder::decodeAndOutputPicture(
    webrtc::scoped_refptr<AV1Picture> pic,
    const libgav1::Vector<libgav1::TileBuffer>& tileBuffers) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!checkAndCleanUpReferenceFrames()) {
    LOG_ERROR("[AV1Decoder] decodeAndOutputPicture: ref frames differ from state");
    return AV1Accelerator::Status::kFail;
  }
  const AV1Accelerator::Status status =
      accelerator->submitDecode(*pic, *currentSequenceHeader, refFrames,
                                 tileBuffers,
                                 base::span<const uint8_t>(decoderBuffer->data(),
                                                           decoderBuffer->size()));
  if (status != AV1Accelerator::Status::kOk) {
    if (status == AV1Accelerator::Status::kTryAgain)
      pendingPic = std::move(pic);
    return status;
  }

  if (pic->frameHeader.show_frame && !accelerator->outputPicture(*pic))
    return AV1Accelerator::Status::kFail;

  // |currentFrameHeader->refresh_frame_flags| should be 0xff if the frame is
  // either a SWITCH_FRAME or a visible KEY_FRAME (Spec 5.9.2).
  updateReferenceFrames(std::move(pic));
  return AV1Accelerator::Status::kOk;
}

std::optional<gfx::HDRMetadata> AV1Decoder::GetHDRMetadata() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return hdrMetadata;
}

gfx::Size AV1Decoder::GetPicSize() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // TODO(hiroh): It should be safer to align this by 64 or 128 (depending on
  // use_128x128_superblock) so that a driver doesn't touch out of the buffer.
  return frameSize;
}

gfx::Rect AV1Decoder::GetVisibleRect() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return visibleRect;
}

VideoCodecProfile AV1Decoder::GetProfile() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return profile;
}

uint8_t AV1Decoder::GetBitDepth() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return bitDepth;
}

VideoChromaSampling AV1Decoder::GetChromaSampling() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return chromaSampling;
}

VideoColorSpace AV1Decoder::GetVideoColorSpace() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return pictureColorSpace;
}

size_t AV1Decoder::GetRequiredNumOfPictures() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  constexpr size_t kPicsInPipeline = limits::kMaxVideoFrames + 1;
  return (kPicsInPipeline + GetNumReferenceFrames()) *
         (1 + currentSequenceHeader->film_grain_params_present);
}

size_t AV1Decoder::GetNumReferenceFrames() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return libgav1::kNumReferenceFrameTypes;
}
}  // namespace media
