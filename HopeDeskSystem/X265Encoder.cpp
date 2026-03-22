#include "X265Encoder.h"
#include <api/video/encoded_image.h>
#include <api/video/video_frame.h>

#include "Utils.h"

namespace hope {
    namespace rtc {

        X265Encoder::X265Encoder() {}

        X265Encoder::~X265Encoder() {
            Release();
        }

        int X265Encoder::InitEncode(const webrtc::VideoCodec* codecSettings,
            const webrtc::VideoEncoder::Settings& settings) {

            if (!codecSettings) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

            Release();

            x265Param = x265_param_alloc();
            x265_param_default_preset(x265Param, "ultrafast", "zerolatency");

            x265Param->sourceWidth = codecSettings->width;
            x265Param->sourceHeight = codecSettings->height;
            x265Param->fpsNum = 60;
            x265Param->fpsDenom = 1;
            x265Param->searchMethod = X265_HEX_SEARCH;
            x265Param->rc.rateControlMode = X265_RC_CRF;

            x265Param->lookaheadDepth = 0;      // 关键！默认可能有20-40帧延迟
            x265Param->bFrameAdaptive = 0;      // 禁用自适应B帧决策
            x265Param->rc.cuTree = 0;

            x265Param->rc.aqMode = X265_AQ_NONE;
            x265Param->rc.aqStrength = 1.0; // 强度保持在 1.0 左右

            x265Param->bEnableSAO = 0;

            x265Param->bframes = 0;
            x265Param->keyframeMax = 250;
            x265Param->bRepeatHeaders = 1;

            x265Param->bEnableWeightedPred = 0;
            x265Param->bEnableWeightedBiPred = 0;

            x265Param->maxNumReferences = 2;

            x265Param->bEnableLoopFilter = 0;  // 或者保持开启，设为0节省少量时间

            // 6. 限制帧间预测模式（关键优化！）
            x265Param->limitModes = 1;  // 限制模式选择，加速决策

            // 7. 使用更快的运动估计子像素精度
            x265Param->subpelRefine = 0;  // 0=最快，2=平衡，4=质量优先

            // 8. 禁用早期跳过检测的复杂分析
            x265Param->bEnableEarlySkip = 1;  // 启用早期跳过

            // 9. 禁用RDO优化（大幅提速，明显降质）
            x265Param->rdLevel = 0;  

            x265EncoderInstance = x265_encoder_open(x265Param);
            if (!x265EncoderInstance) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }

            pictureIn = x265_picture_alloc();
            x265_picture_init(x265Param, pictureIn);

            return WEBRTC_VIDEO_CODEC_OK;
        }

        int X265Encoder::Release() {
            if (x265EncoderInstance) {
                x265_encoder_close(x265EncoderInstance);
                x265EncoderInstance = nullptr;
            }
            if (pictureIn) {
                x265_picture_free(pictureIn);
                pictureIn = nullptr;
            }
            if (x265Param) {
                x265_param_free(x265Param);
                x265Param = nullptr;
            }
            return WEBRTC_VIDEO_CODEC_OK;
        }

        int X265Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
            encodedImageCallback = callback;
            return WEBRTC_VIDEO_CODEC_OK;
        }

        int X265Encoder::Encode(const webrtc::VideoFrame& frame,
            const std::vector<webrtc::VideoFrameType>* frameTypes) {

            if (!x265EncoderInstance || !encodedImageCallback) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

            bool forceKeyFrame = false;
            if (frameTypes) {
                for (auto type : *frameTypes) {
                    if (type == webrtc::VideoFrameType::kVideoFrameKey) {
                        forceKeyFrame = true;
                        break;
                    }
                }
            }

            webrtc::scoped_refptr<webrtc::I420BufferInterface> i420Buffer = frame.video_frame_buffer()->ToI420();

            pictureIn->planes[0] = (void*)i420Buffer->DataY();
            pictureIn->planes[1] = (void*)i420Buffer->DataU();
            pictureIn->planes[2] = (void*)i420Buffer->DataV();
            pictureIn->stride[0] = i420Buffer->StrideY();
            pictureIn->stride[1] = i420Buffer->StrideU();
            pictureIn->stride[2] = i420Buffer->StrideV();
            pictureIn->colorSpace = X265_CSP_I420;
            pictureIn->sliceType = forceKeyFrame ? X265_TYPE_IDR : X265_TYPE_AUTO;
            pictureIn->pts = frame.render_time_ms();

            x265_nal* nals = nullptr;
            uint32_t numNals = 0;

            int frameSize = x265_encoder_encode(x265EncoderInstance, &nals, &numNals, pictureIn, nullptr);
   
            if (frameSize > 0 && numNals > 0) {
                encodedBuffer.clear();
                for (uint32_t i = 0; i < numNals; ++i) {
                    encodedBuffer.insert(encodedBuffer.end(),
                        nals[i].payload,
                        nals[i].payload + nals[i].sizeBytes);
                }

                webrtc::EncodedImage encodedImage;
                encodedImage.SetEncodedData(webrtc::EncodedImageBuffer::Create(
                    encodedBuffer.data(), encodedBuffer.size()));

                encodedImage._encodedWidth = x265Param->sourceWidth;
                encodedImage._encodedHeight = x265Param->sourceHeight;
                encodedImage._frameType = (nals[0].type >= 16 && nals[0].type <= 21) ?
                    webrtc::VideoFrameType::kVideoFrameKey :
                    webrtc::VideoFrameType::kVideoFrameDelta;

                encodedImage.SetRtpTimestamp(frame.rtp_timestamp());
                encodedImage.capture_time_ms_ = frame.render_time_ms();

                webrtc::CodecSpecificInfo info;
                info.codecType = webrtc::kVideoCodecH265;

                encodedImageCallback->OnEncodedImage(encodedImage, &info);
            }

            return WEBRTC_VIDEO_CODEC_OK;
        }

        void X265Encoder::SetRates(const RateControlParameters& parameters) {

        }

        webrtc::VideoEncoder::EncoderInfo X265Encoder::GetEncoderInfo() const {
            EncoderInfo info;
            info.supports_native_handle = false;
            info.is_hardware_accelerated = false;
            info.implementation_name = "X265HEVC";
            return info;
        }
    }
}