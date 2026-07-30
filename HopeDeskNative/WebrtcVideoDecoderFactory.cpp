#include "WebrtcVideoDecoderFactory.h"

#include <media/base/media_constants.h>
#include <modules/video_coding/codecs/h264/include/h264.h>
#include <modules/video_coding/codecs/vp8/include/vp8.h>
#include <modules/video_coding/codecs/vp9/include/vp9.h>
#include <modules/video_coding/codecs/av1/dav1d_decoder.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <functional>

#include "De265Decoder.h"
#include "NvdecDecoder.h"
#include "Utils.h"

namespace hope {

	namespace rtc{

        // 包装解码器:优先硬解,Configure 失败则运行时退回 WebRTC 原生软解。
        class FallbackDecoder : public webrtc::VideoDecoder {
        public:
            using SoftCreator = std::function<std::unique_ptr<webrtc::VideoDecoder>()>;

            FallbackDecoder(std::unique_ptr<webrtc::VideoDecoder> hard, SoftCreator softCreator,
                            std::string codec, std::function<void(const std::string&, bool)> statusHandle)
                : hard(std::move(hard)), softCreator(std::move(softCreator)),
                  codec(std::move(codec)), statusHandle(std::move(statusHandle)) {}

            int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* cb) override {
                handle = cb;
                if (hard) hard->RegisterDecodeCompleteCallback(cb);
                if (soft) soft->RegisterDecodeCompleteCallback(cb);
                return WEBRTC_VIDEO_CODEC_OK;
            }

            bool Configure(const Settings& settings) override {
                if (hard && hard->Configure(settings)) {
                    active = kHard;
                    LOG_INFO("解码: 硬件解码(NVDEC/CUDA) 已启用");
                    if (statusHandle) statusHandle(codec, true);
                    return true;
                }
                // 硬解失败 -> 退回 WebRTC 原生软解
                hard.reset();
                LOG_WARN("硬解 Configure 失败,回退 WebRTC 软解");
                soft = softCreator();
                if (soft && soft->Configure(settings)) {
                    active = kSoft;
                    LOG_INFO("解码: 软件解码 已启用(硬解回退)");
                    if (statusHandle) statusHandle(codec, false);
                    return true;
                }
                LOG_ERROR("软解 Configure 也失败,无可用解码器");
                return false;
            }

            int32_t Decode(const webrtc::EncodedImage& image, bool missingFrames, int64_t renderTimeMs) override {
                if (active == kHard && hard) return hard->Decode(image, missingFrames, renderTimeMs);
                if (active == kSoft && soft) return soft->Decode(image, missingFrames, renderTimeMs);
                return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            }

            int32_t Release() override {
                if (hard) hard->Release();
                if (soft) soft->Release();
                return WEBRTC_VIDEO_CODEC_OK;
            }

            DecoderInfo GetDecoderInfo() const override {
                if (active == kHard && hard) return hard->GetDecoderInfo();
                if (active == kSoft && soft) return soft->GetDecoderInfo();
                return DecoderInfo();
            }

        private:
            enum Active { kNone, kHard, kSoft } active = kNone;
            std::unique_ptr<webrtc::VideoDecoder> hard;
            SoftCreator softCreator;
            std::unique_ptr<webrtc::VideoDecoder> soft;
            std::string codec;
            std::function<void(const std::string&, bool)> statusHandle;
            webrtc::DecodedImageCallback* handle = nullptr;
        };

        WebrtcVideoDecoderFactory::WebrtcVideoDecoderFactory():internalDecoderFactory(new webrtc::InternalDecoderFactory()) {
		
		}

        std::unique_ptr<webrtc::VideoDecoder> WebrtcVideoDecoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {
            LOG_INFO("DecoderFactory::Create format=%s webrtcEnableNvdec=%d",
                     format.name.c_str(), webrtcEnableNvdec);

            if (!format.IsCodecInList(GetSupportedFormats())) {

                LOG_WARN("Trying to create decoder for unsupported format: %s", format.ToString().c_str());

                return nullptr;
            }

            // WebRTC 原生软解构造器(按值捕获 env/format,供 Configure 阶段延迟使用)
            auto makeSoft = [env, format]() -> std::unique_ptr<webrtc::VideoDecoder> {
                if (absl::EqualsIgnoreCase(format.name, webrtc::kVp8CodecName))
                    return webrtc::CreateVp8Decoder(env);
                if (absl::EqualsIgnoreCase(format.name, webrtc::kVp9CodecName))
                    return webrtc::VP9Decoder::Create();
                if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName))
                    return webrtc::H264Decoder::Create();
                if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName))
                    return webrtc::CreateDav1dDecoder(env);
                if (absl::EqualsIgnoreCase(format.name, "H265") || absl::EqualsIgnoreCase(format.name, "HEVC"))
                    return std::make_unique<De265Decoder>(env);
                return nullptr;
            };

            // 硬解开关:满足 codec 则优先硬解(FallbackDecoder 在 Configure 失败时
            // 运行时退回软解;能力探测在 NvdecDecoder::Configure 内做,无 N 卡/驱动
            // 时 EnsureContext 失败即回退)。
            if (webrtcEnableNvdec) {
                NvdecDecoder::Codec nvdecCodec = NvdecDecoder::Codec::H264;
                bool codecOk = false;
                if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName)) { nvdecCodec = NvdecDecoder::Codec::H264; codecOk = true; }
                else if (absl::EqualsIgnoreCase(format.name, "H265") || absl::EqualsIgnoreCase(format.name, "HEVC")) { nvdecCodec = NvdecDecoder::Codec::H265; codecOk = true; }
                else if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName)) { nvdecCodec = NvdecDecoder::Codec::AV1; codecOk = true; }

                if (codecOk) {
                    LOG_INFO("Prefer NVDEC hardware decode for %s (fallback to soft on failure)", format.name.c_str());
                    auto hard = std::make_unique<NvdecDecoder>(nvdecCodec);
                    return std::make_unique<FallbackDecoder>(std::move(hard), makeSoft,
                                                              format.name, onDecoderStatusHandle);
                }
            }

            // 否则直接 WebRTC 原生软解
            if (webrtcEnableNvdec) {
                LOG_INFO("解码: 软件解码 已启用(format=%s,该 codec 无硬解路径)", format.name.c_str());
            } else {
                LOG_INFO("解码: 软件解码 已启用(format=%s,硬件解码未开启)", format.name.c_str());
            }
            if (onDecoderStatusHandle) onDecoderStatusHandle(format.name, false);
            return makeSoft();
        }

        webrtc::VideoDecoderFactory::CodecSupport WebrtcVideoDecoderFactory::QueryCodecSupport(
            const webrtc::SdpVideoFormat& format,
            bool reference_scaling) const {

            LOG_INFO("format Support:%s", format.name.c_str());

            if (format.name == "H265" || format.name == "HEVC") {

                CodecSupport codecSupport;

                codecSupport.is_supported = true;

                codecSupport.is_power_efficient = true;

                return codecSupport;
            }

            return internalDecoderFactory->QueryCodecSupport(format,
                reference_scaling);
        }

        std::vector<webrtc::SdpVideoFormat> WebrtcVideoDecoderFactory::GetSupportedFormats() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalDecoderFactory->GetSupportedFormats();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }

	}

}
