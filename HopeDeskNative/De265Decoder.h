#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>

#include "api/environment/environment.h"
#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_decoder.h"
#include "modules/video_coding/include/video_error_codes.h"

// Include libde265
#include "libde265/de265.h"

namespace hope {
namespace rtc {

constexpr char kDe265Name[] = "libde265";
constexpr int kMaxDe265Threads = 16;

class De265Decoder : public webrtc::VideoDecoder {
 public:
  De265Decoder();
  explicit De265Decoder(const webrtc::Environment& env);
  De265Decoder(const De265Decoder&) = delete;
  De265Decoder& operator=(const De265Decoder&) = delete;

  ~De265Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& encodedImage,
                 int64_t renderTimeMs) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;
  const char* ImplementationName() const override;

 private:
  de265_decoder_context* decoderContext = nullptr;
  webrtc::DecodedImageCallback* decodeCompleteCallback = nullptr;

  const bool cropToRenderResolution = false;
};

}
}
