// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_AV1_PICTURE_H_
#define MEDIA_GPU_AV1_PICTURE_H_

#include "chromiumShims/MediaCodecPicture.h"
#include "chromiumShims/MediaGpuExport.h"
#include "chromiumShims/MediaSvcGenericMetadata.h"
#include "libgav1/utils/types.h"

namespace media {

// AV1Picture carries the parsed frame header needed for decoding an AV1 frame.
// It also owns the decoded frame itself.
class MEDIA_GPU_EXPORT AV1Picture : public CodecPicture {
 public:
  AV1Picture();
  AV1Picture(const AV1Picture&) = delete;
  AV1Picture& operator=(const AV1Picture&) = delete;

  // Create a duplicate instance and copy the data to it. It is used to support
  // the AV1 show_existing_frame feature. Return the scoped_refptr pointing to
  // the duplicate instance, or nullptr on failure.
  webrtc::scoped_refptr<AV1Picture> duplicate();

  libgav1::ObuFrameHeader frameHeader = {};

  std::optional<SVCGenericMetadata> svcGeneric;

 protected:
  ~AV1Picture() override;

 private:
  // Create a duplicate instance.
  virtual webrtc::scoped_refptr<AV1Picture> createDuplicate();
};
}  // namespace media
#endif  // MEDIA_GPU_AV1_PICTURE_H_
