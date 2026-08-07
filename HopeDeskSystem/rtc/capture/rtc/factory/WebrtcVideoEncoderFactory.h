#pragma once
#include <functional>
#include <string>
#include <memory>
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "../capture/VddChannelSync.h"

namespace hope {

	namespace rtc {
	
		class WebrtcVideoEncoderFactory : public webrtc::VideoEncoderFactory {

		public:

			WebrtcVideoEncoderFactory();

			std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,const webrtc::SdpVideoFormat& format) override;

			webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
				const webrtc::SdpVideoFormat& format,
				std::optional<std::string> scalability_mode) const;

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

			std::vector<webrtc::SdpVideoFormat> GetImplementations() const override;

		public:

			int webrtcEnableNvenc = 0;

			// 编码器创建后回调:告知本次实际用的是硬编(NVENC)还是软编 + codec,
			// WebrtcManager 据此经本地 TCP(ENCODE_STATUS)上报给被控端 Native 显示。
			std::function<void(const std::string& codec, bool hardEncode)> onEncoderStatusHandle;

			// 与上游 VirtualDisplayCapture 共享的 VDD 帧通道同步状态。
			// 创建 NVENC 编码器时注入，keyed-mutex 同步丢失时驱动自愈。
			std::shared_ptr<VddChannelSync> channelSync;

		private:

			const std::unique_ptr<VideoEncoderFactory> internalEncoderFactory;

		};

	}

}

