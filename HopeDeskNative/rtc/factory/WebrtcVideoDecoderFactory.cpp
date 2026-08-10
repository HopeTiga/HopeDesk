#include "WebrtcVideoDecoderFactory.h"

#include <media/base/media_constants.h>
#include <modules/video_coding/codecs/h264/include/h264.h>
#include <modules/video_coding/codecs/vp8/include/vp8.h>
#include <modules/video_coding/codecs/vp9/include/vp9.h>
#include <modules/video_coding/codecs/av1/dav1d_decoder.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <functional>
#include <algorithm>

#include "../codec/De265Decoder.h"
#include "../codec/NvdecDecoder.h"
#include "../codec/D3D11Av1VideoDecoder.h"
#include "../../utils/Utils.h"

namespace hope {

	namespace rtc{

        // 包装解码器:优先硬解,Configure 失败或运行时连续解码失败则退回 WebRTC 原生软解。
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
                this->settings = settings;
                if (hard && hard->Configure(settings)) {
                    active = kHard;
                    hardErrors = 0;
                    LOG_INFO("解码: 硬件解码 已启用");
                    if (statusHandle) statusHandle(codec, true);
                    return true;
                }
                // 硬解失败 -> 退回 WebRTC 原生软解
                hard.reset();
                LOG_WARN("硬解 Configure 失败,回退 WebRTC 软解");
                return switchToSoft();
            }

            int32_t Decode(const webrtc::EncodedImage& image, bool missingFrames, int64_t renderTimeMs) override {
                if (active == kHard && hard) {
                    int32_t ret = hard->Decode(image, missingFrames, renderTimeMs);
                    if (ret == WEBRTC_VIDEO_CODEC_ERROR) {
                        // 硬解运行时连续失败(设备移除/解码错误) -> 回退软解。
                        if (++hardErrors >= kMaxHardErrors) {
                            LOG_WARN("硬解连续 %d 次解码失败,运行时回退软解", kMaxHardErrors);
                            switchToSoft();
                        }
                    } else {
                        hardErrors = 0;
                    }
                    return ret;
                }
                if (active == kSoft && soft) return soft->Decode(image, missingFrames, renderTimeMs);
                return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
            }

            int32_t Release() override {
                if (hard) hard->Release();
                if (soft) soft->Release();
                return WEBRTC_VIDEO_CODEC_OK;
            }

        private:
            bool switchToSoft() {
                if (active == kSoft) return true;
                if (hard) { hard->Release(); hard.reset(); }
                soft = softCreator();
                if (soft && soft->Configure(settings)) {
                    if (handle) soft->RegisterDecodeCompleteCallback(handle);
                    active = kSoft;
                    hardErrors = 0;
                    LOG_INFO("解码: 软件解码 已启用(硬解回退)");
                    if (statusHandle) statusHandle(codec, false);
                    return true;
                }
                LOG_ERROR("软解回退失败,无可用解码器");
                return false;
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
            webrtc::VideoDecoder::Settings settings;   // 运行时回退软解时用
            int hardErrors = 0;                        // 硬解连续解码错误计数
            static constexpr int kMaxHardErrors = 5;   // 超过则运行时回退软解
        };

        WebrtcVideoDecoderFactory::WebrtcVideoDecoderFactory():internalDecoderFactory(new webrtc::InternalDecoderFactory()) {

		}

        void WebrtcVideoDecoderFactory::setDecoderD3D11Device(ID3D11Device* dev) {
            std::lock_guard<std::mutex> lock(decoderMutex);
            decoderD3D11Device = dev;
            // 转发给所有已创建且存活的解码器(可能先于 VideoWidget 初始化)。
            for (auto* d : liveAv1Decoders) {
                if (d) d->setD3D11Device(dev);
            }
        }

        void WebrtcVideoDecoderFactory::clearDecoderD3D11Device() {
            std::lock_guard<std::mutex> lock(decoderMutex);
            decoderD3D11Device.Reset();
        }

        void WebrtcVideoDecoderFactory::wakeUpAllDecoders() {
            std::lock_guard<std::mutex> lock(decoderMutex);
            for (auto* d : liveAv1Decoders) {
                if (d) d->requestRelease();
            }
        }

        void WebrtcVideoDecoderFactory::removeDecoder(D3D11Av1VideoDecoder* d) {
            std::lock_guard<std::mutex> lock(decoderMutex);
            liveAv1Decoders.erase(
                std::remove(liveAv1Decoders.begin(), liveAv1Decoders.end(), d),
                liveAv1Decoders.end());
        }

        void WebrtcVideoDecoderFactory::setOnDisplayHandle(std::function<void(std::shared_ptr<VideoFrame>)> onDisplayHandle) {
            std::lock_guard<std::mutex> lock(decoderMutex);
            this->onDisplayHandle = std::move(onDisplayHandle);
            // 同步给已创建的硬解解码器(可能先于回调注入创建,否则帧被 sink 跳过)。
            for (D3D11Av1VideoDecoder* decoder : liveAv1Decoders) {
                if (decoder) decoder->setOnDisplayHandle(this->onDisplayHandle);
            }
            for (NvdecDecoder* decoder : liveNvdecDecoders) {
                if (decoder) decoder->setOnDisplayHandle(this->onDisplayHandle);
            }
        }

        void WebrtcVideoDecoderFactory::removeNvdecDecoder(NvdecDecoder* decoder) {
            std::lock_guard<std::mutex> lock(decoderMutex);
            liveNvdecDecoders.erase(
                std::remove(liveNvdecDecoders.begin(), liveNvdecDecoders.end(), decoder),
                liveNvdecDecoders.end());
        }

        std::unique_ptr<webrtc::VideoDecoder> WebrtcVideoDecoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {
            LOG_INFO("DecoderFactory::Create format=%s webrtcEnableD3D11=%d",
                     format.name.c_str(), webrtcEnableD3D11);

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
            // 运行时退回软解)。
            //   - AV1: D3D11Av1VideoDecoder(DXVA 直连,不依赖商店 AV1 扩展)
            //   - H264/HEVC: NvdecDecoder(NVDEC/CUVID)
            if (webrtcEnableD3D11) {
                const bool isAv1 = absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName);
                const bool isH264 = absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName);
                const bool isH265 = absl::EqualsIgnoreCase(format.name, "H265") ||
                                    absl::EqualsIgnoreCase(format.name, "HEVC");

                if (isAv1) {
                    LOG_INFO("Prefer D3D11(AV1/DXVA) hardware decode for %s (fallback to soft on failure)", format.name.c_str());
                    std::unique_ptr<D3D11Av1VideoDecoder> hardDecoder = std::make_unique<D3D11Av1VideoDecoder>();
                    {
                        std::lock_guard<std::mutex> lock(decoderMutex);
                        hardDecoder->setOwnerFactory(this);
                        hardDecoder->setOnDisplayHandle(onDisplayHandle);
                        liveAv1Decoders.push_back(hardDecoder.get());
                    }
                    if (decoderD3D11Device) hardDecoder->setD3D11Device(decoderD3D11Device.Get());
                    return std::make_unique<FallbackDecoder>(std::move(hardDecoder), makeSoft,
                                                              format.name, onDecoderStatusHandle);
                }
                if (isH264 || isH265) {
                    NvdecDecoder::Codec nvdecCodec =
                        isH264 ? NvdecDecoder::Codec::H264 : NvdecDecoder::Codec::H265;
                    LOG_INFO("Prefer NVDEC hardware decode for %s (fallback to soft on failure)", format.name.c_str());
                    std::unique_ptr<NvdecDecoder> hardDecoder = std::make_unique<NvdecDecoder>(nvdecCodec);
                    {
                        std::lock_guard<std::mutex> lock(decoderMutex);
                        hardDecoder->setOwnerFactory(this);
                        hardDecoder->setOnDisplayHandle(onDisplayHandle);
                        liveNvdecDecoders.push_back(hardDecoder.get());
                    }
                    return std::make_unique<FallbackDecoder>(std::move(hardDecoder), makeSoft,
                                                              format.name, onDecoderStatusHandle);
                }
            }

            // 否则直接 WebRTC 原生软解
            if (webrtcEnableD3D11) {
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
