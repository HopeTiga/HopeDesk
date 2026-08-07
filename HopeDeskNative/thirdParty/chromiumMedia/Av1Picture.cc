// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Av1Picture.h"

#include <memory>

#include "chromiumShims/BaseHelpers.h"
#include "chromiumShims/MediaUtils.h"

namespace media {
AV1Picture::AV1Picture() = default;
AV1Picture::~AV1Picture() = default;

webrtc::scoped_refptr<AV1Picture> AV1Picture::duplicate() {
  webrtc::scoped_refptr<AV1Picture> dupPic = createDuplicate();
  if (!dupPic)
    return nullptr;

  // Copy members of AV1Picture and CodecPicture.
  // A proper bitstream id is set in AV1Decoder.
  // Note that decrypt_config_ is not used in here, so skip copying it.
  dupPic->frameHeader = frameHeader;
  dupPic->set_bitstream_id(bitstream_id());
  dupPic->set_visible_rect(visible_rect());
  dupPic->set_colorspace(get_colorspace());
  return dupPic;
}

webrtc::scoped_refptr<AV1Picture> AV1Picture::createDuplicate() {
  return base::MakeRefCounted<AV1Picture>();
}

}  // namespace media
