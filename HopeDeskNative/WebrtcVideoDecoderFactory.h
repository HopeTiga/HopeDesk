#pragma once
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "media/engine/internal_decoder_factory.h"

#include <functional>
#include <string>

namespace hope {

	namespace rtc {

        class WebrtcVideoDecoderFactory : public webrtc::VideoDecoderFactory {

		public:

            WebrtcVideoDecoderFactory();

			std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;
			
			CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
				bool reference_scaling) const;

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;


		public:

			int webrtcEnableNvdec = 0;

			// 解码状态 handle:(codec 名, 是否硬解)。Configure 后触发,供 UI 显示。
			std::function<void(const std::string& codec, bool hardDecode)> onDecoderStatusHandle;

			// NvdecDecoder 创建后回调(传 raw 指针,不持有),供 WebrtcManager 拿去做零拷贝 setup。
			std::function<void(class NvdecDecoder*)> onNvdecCreated;

		private:

			std::unique_ptr<webrtc::VideoDecoderFactory> internalDecoderFactory;

		};

	}

}

