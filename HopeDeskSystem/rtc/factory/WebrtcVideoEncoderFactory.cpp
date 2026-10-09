#include "WebrtcVideoEncoderFactory.h"

#include <media/engine/simulcast_encoder_adapter.h>

#include <chrono>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "../encoder/NvencAV1Encoder.h"
#include "../encoder/NvencH265Encoder.h"
#include "../encoder/X265Encoder.h"
#include "../../utils/Utils.h"
#include "WebrtcVideoDecoderFactory.h"

namespace hope {

	namespace rtc
	{

        class FallbackEncoder : public webrtc::VideoEncoder {
        public:
            FallbackEncoder(std::unique_ptr<webrtc::VideoEncoder> hardEncoder,
                std::unique_ptr<webrtc::VideoEncoder> softEncoder,
                std::string codec,
                std::function<void(const std::string&, bool)> statusHandle)
                : hardEncoder(std::move(hardEncoder)), softEncoder(std::move(softEncoder)),
                codec(std::move(codec)), statusHandle(std::move(statusHandle)) {
            }

            int InitEncode(const webrtc::VideoCodec* codecSettings,
                const webrtc::VideoEncoder::Settings& settings) override {

                if (!codecSettings) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

                this->codecSettings = *codecSettings;
                this->encoderSettings = settings;
                rateControlParameters.reset();
                active = kNone;

                int result = WEBRTC_VIDEO_CODEC_ERROR;
                if (hardEncoder) {
                    result = hardEncoder->InitEncode(codecSettings, settings);
                    if (result == WEBRTC_VIDEO_CODEC_OK) {
                        active = kHard;
                        hardErrors = 0;
                        primeEncoder();
                        LOG_INFO("Hardware Encoder Enabled Codec={}", codec);
                        if (statusHandle) statusHandle(codec, true);
                        return WEBRTC_VIDEO_CODEC_OK;
                    }
                    LOG_WARN("Hardware Encoder InitEncode Failed Codec={} Code={}, Fallback To Software", codec, result);
                }

                if (!switchToSoft()) return result;
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
                this->callback = callback;
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (!encoder) return WEBRTC_VIDEO_CODEC_ERROR;
                return encoder->RegisterEncodeCompleteCallback(callback);
            }

            int Release() override {
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (encoder) encoder->Release();
                active = kNone;
                return WEBRTC_VIDEO_CODEC_OK;
            }

            int Encode(const webrtc::VideoFrame& frame,
                const std::vector<webrtc::VideoFrameType>* frameTypes) override {

                if (active == kHard && hardEncoder) {
                    const int result = hardEncoder->Encode(frame, frameTypes);
                    if (result == WEBRTC_VIDEO_CODEC_OK) {
                        hardErrors = 0;
                        return WEBRTC_VIDEO_CODEC_OK;
                    }

                    if (hardErrors == 0) hardErrorSince = std::chrono::steady_clock::now();
                    ++hardErrors;
                    const int64_t failedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - hardErrorSince).count();
                    if (hardErrors < kMaxHardErrors || failedMs < kMinHardErrorMs) return result;

                    LOG_WARN("Hardware Encoder Failed {} Times In {} ms, Fallback To Software", hardErrors, failedMs);
                    if (!switchToSoft()) return result;
                }

                if (active != kSoft || !softEncoder) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

                if (frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative &&
                    !softEncoder->GetEncoderInfo().supports_native_handle) {
                    if (!nativeFrameDropped) {
                        nativeFrameDropped = true;
                        LOG_WARN("Software Encoder Does Not Support Native Frame, Drop Until Capture Switched To CPU Path");
                    }
                    return WEBRTC_VIDEO_CODEC_OK;
                }

                std::vector<webrtc::VideoFrameType> forcedFrameTypes;
                if (pendingKeyframe) {
                    pendingKeyframe = false;
                    forcedFrameTypes.push_back(webrtc::VideoFrameType::kVideoFrameKey);
                    frameTypes = &forcedFrameTypes;
                }

                return softEncoder->Encode(frame, frameTypes);
            }

            void SetRates(const RateControlParameters& parameters) override {
                rateControlParameters = parameters;
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (encoder) encoder->SetRates(parameters);
            }

            void SetFecControllerOverride(webrtc::FecControllerOverride* fecControllerOverride) override {
                if (hardEncoder) hardEncoder->SetFecControllerOverride(fecControllerOverride);
                if (softEncoder) softEncoder->SetFecControllerOverride(fecControllerOverride);
            }

            void OnPacketLossRateUpdate(float packetLossRate) override {
                this->packetLossRate = packetLossRate;
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (encoder) encoder->OnPacketLossRateUpdate(packetLossRate);
            }

            void OnRttUpdate(int64_t rttMs) override {
                this->rttMs = rttMs;
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (encoder) encoder->OnRttUpdate(rttMs);
            }

            void OnLossNotification(const LossNotification& lossNotification) override {
                this->lossNotification = lossNotification;
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (encoder) encoder->OnLossNotification(lossNotification);
            }

            EncoderInfo GetEncoderInfo() const override {
                EncoderInfo info;
                if (active == kSoft && softEncoder) {
                    info = softEncoder->GetEncoderInfo();
                    info.is_hardware_accelerated = false;
                }
                else if (hardEncoder) {
                    info = hardEncoder->GetEncoderInfo();
                }
                else if (softEncoder) {
                    info = softEncoder->GetEncoderInfo();
                    info.is_hardware_accelerated = false;
                }

                if (hardEncoder && softEncoder) {
                    const EncoderInfo hardInfo = hardEncoder->GetEncoderInfo();
                    const EncoderInfo softInfo = softEncoder->GetEncoderInfo();
                    info.requested_resolution_alignment = std::lcm(
                        hardInfo.requested_resolution_alignment, softInfo.requested_resolution_alignment);
                    info.apply_alignment_to_all_simulcast_layers =
                        hardInfo.apply_alignment_to_all_simulcast_layers ||
                        softInfo.apply_alignment_to_all_simulcast_layers;
                }

                return info;
            }

        private:

            webrtc::VideoEncoder* currentEncoder() const {
                if (active == kSoft && softEncoder) return softEncoder.get();
                if (hardEncoder) return hardEncoder.get();
                return softEncoder.get();
            }

            void primeEncoder() {
                webrtc::VideoEncoder* encoder = currentEncoder();
                if (!encoder) return;
                if (callback) encoder->RegisterEncodeCompleteCallback(callback);
                if (rateControlParameters) encoder->SetRates(*rateControlParameters);
                if (packetLossRate) encoder->OnPacketLossRateUpdate(*packetLossRate);
                if (rttMs) encoder->OnRttUpdate(*rttMs);
                if (lossNotification) encoder->OnLossNotification(*lossNotification);
            }

            bool switchToSoft() {
                if (!softEncoder) return false;

                if (hardEncoder) {
                    hardEncoder->Release();
                    hardEncoder.reset();
                }

                if (softEncoder->InitEncode(&codecSettings, encoderSettings.value()) != WEBRTC_VIDEO_CODEC_OK) {
                    LOG_ERROR("Software Fallback InitEncode Failed Codec={}", codec);
                    return false;
                }

                active = kSoft;
                hardErrors = 0;
                pendingKeyframe = true;
                primeEncoder();
                LOG_INFO("Software Encoder Enabled Codec={}", codec);
                if (statusHandle) statusHandle(codec, false);
                return true;
            }

            enum Active { kNone, kHard, kSoft };

            Active active = kNone;
            std::unique_ptr<webrtc::VideoEncoder> hardEncoder;
            std::unique_ptr<webrtc::VideoEncoder> softEncoder;
            std::string codec;
            std::function<void(const std::string&, bool)> statusHandle;

            webrtc::EncodedImageCallback* callback = nullptr;
            webrtc::VideoCodec codecSettings;
            std::optional<webrtc::VideoEncoder::Settings> encoderSettings;
            std::optional<RateControlParameters> rateControlParameters;
            std::optional<float> packetLossRate;
            std::optional<int64_t> rttMs;
            std::optional<LossNotification> lossNotification;

            int hardErrors = 0;
            std::chrono::steady_clock::time_point hardErrorSince;
            bool pendingKeyframe = false;
            bool nativeFrameDropped = false;

            static constexpr int kMaxHardErrors = 5;
            static constexpr int kMinHardErrorMs = 2000;
        };

        namespace {

            std::unique_ptr<webrtc::VideoEncoder> createSoftEncoder(const webrtc::Environment& env,
                const webrtc::SdpVideoFormat& format,
                webrtc::VideoEncoderFactory* internalEncoderFactory) {

                if (format.name == "H265" || format.name == "h265" || format.name == "HEVC") {
                    return std::make_unique<X265Encoder>();
                }

                if (format.IsCodecInList(internalEncoderFactory->GetSupportedFormats())) {
                    return std::make_unique<webrtc::SimulcastEncoderAdapter>(
                        env, internalEncoderFactory, nullptr, format);
                }

                return nullptr;
            }

            std::unique_ptr<webrtc::VideoEncoder> wrapWithSoftFallback(const webrtc::Environment& env,
                const webrtc::SdpVideoFormat& format,
                webrtc::VideoEncoderFactory* internalEncoderFactory,
                std::unique_ptr<webrtc::VideoEncoder> hardEncoder,
                const std::function<void(const std::string&, bool)>& statusHandle) {

                std::unique_ptr<webrtc::VideoEncoder> softEncoder =
                    createSoftEncoder(env, format, internalEncoderFactory);

                if (!softEncoder) {
                    LOG_WARN("No Software Fallback Encoder Format={}", format.name);
                    return hardEncoder;
                }

                return std::make_unique<FallbackEncoder>(std::move(hardEncoder), std::move(softEncoder),
                    format.name, statusHandle);
            }

        }

        WebrtcVideoEncoderFactory::WebrtcVideoEncoderFactory() :internalEncoderFactory(new webrtc::InternalEncoderFactory()) {

        }

        std::unique_ptr<webrtc::VideoEncoder> WebrtcVideoEncoderFactory::Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format)
        {

            if ((format.name == "AV1" || format.name == "av1") && webrtcEnableNvenc == 1) {

                LOG_INFO("NvencAV1Encoder");
                std::unique_ptr<NvencAV1Encoder> encoder = std::make_unique<NvencAV1Encoder>();
                encoder->SetChannelSync(channelSync);
                return wrapWithSoftFallback(env, format, internalEncoderFactory.get(),
                    std::move(encoder), onEncoderStatusHandle);

            }

            if ((format.name == "H265" || format.name == "h265") && webrtcEnableNvenc == 1) {

                LOG_INFO("NvencH265Encoder");
                std::unique_ptr<NvencH265Encoder> encoder = std::make_unique<NvencH265Encoder>();
                encoder->SetChannelSync(channelSync);
                return wrapWithSoftFallback(env, format, internalEncoderFactory.get(),
                    std::move(encoder), onEncoderStatusHandle);

            }
            else if (format.name == "H265" || format.name == "h265") {

                LOG_INFO("X265Encoder");
                if (onEncoderStatusHandle) onEncoderStatusHandle("H265", false);
                return std::make_unique<X265Encoder>();

            }

            if (format.IsCodecInList(
                internalEncoderFactory->GetSupportedFormats())) {
                if (onEncoderStatusHandle) onEncoderStatusHandle(format.name, false);
                return std::make_unique<webrtc::SimulcastEncoderAdapter>(
                    env,
                    /*primary_factory=*/internalEncoderFactory.get(),
                    /*fallback_factory=*/nullptr, format);
            }

            return nullptr;
        }

        webrtc::VideoEncoderFactory::CodecSupport WebrtcVideoEncoderFactory::QueryCodecSupport(
            const webrtc::SdpVideoFormat& format,
            std::optional<std::string> scalability_mode) const {

            LOG_INFO("Format Support:{}",format.name);

            if (format.name == "H265" || format.name == "HEVC") {

                CodecSupport codecSupport;

                codecSupport.is_supported = true;

                codecSupport.is_power_efficient = true;

                return codecSupport;
            }

            return internalEncoderFactory->QueryCodecSupport(format,
                scalability_mode);
        }

        std::vector<webrtc::SdpVideoFormat> WebrtcVideoEncoderFactory::GetSupportedFormats() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalEncoderFactory->GetSupportedFormats();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }

        std::vector<webrtc::SdpVideoFormat> WebrtcVideoEncoderFactory::GetImplementations() const {

            std::vector<webrtc::SdpVideoFormat> sdpVideoFormats = internalEncoderFactory->GetImplementations();

            sdpVideoFormats.emplace_back(webrtc::SdpVideoFormat::H265());

            return sdpVideoFormats;
        }
	}

}
