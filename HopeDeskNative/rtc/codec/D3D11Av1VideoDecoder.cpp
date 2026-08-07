// INITGUID:在本 TU 内定义 dxva.h 里的 DEFINE_GUID(如 DXVA_ModeAV1_VLD_*),
// 避免依赖 dxguid.lib 链接。
#define INITGUID
#include "D3D11Av1VideoDecoder.h"
#include "../factory/WebrtcVideoDecoderFactory.h"
#include "WebrtcD3D11TextureBuffer.h"
#include "D3D11VideoFrameData.h"
#include "../WebrtcManager.h"   // app VideoFrame 完整定义(直投 onDisplayHandle 载荷)
#include "api/make_ref_counted.h"
#include <modules/video_coding/include/video_error_codes.h>

#include "chromiumShims/BaseHelpers.h"
#include "chromiumShims/BaseSpan.h"
#include "libgav1/utils/common.h"
#include "libgav1/utils/constants.h"
#include "libgav1/utils/types.h"

#include <d3d11_3.h>
#include <dxgi.h>   // IDXGIKeyedMutex(解码写/渲染读同步)
#include <chrono>
#include <cstring>
#include <thread>
#include "../../utils/Utils.h"

namespace {
// D3DERR_WASSTILLDRAWING = 0x887601AA(d3d9.h 定义)。不直接 include <d3d9.h>:
// 本 TU 已先 include dxva.h,而 dxva.h 与 d3d9.h 会重复定义 D3DFORMAT/D3DPOOL。
constexpr HRESULT kD3DErrWasStillDrawing = static_cast<HRESULT>(0x887601AAL);
}  // namespace

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace hope {
namespace rtc {

namespace {

// 输出槽位 + AV1Picture 的子类:携带解码输出槽,show_existing_frame 的
// Duplicate() 复用同一槽。
class D3D11Av1Picture : public media::AV1Picture {
public:
    D3D11Av1Picture(D3D11Av1VideoDecoder* decoder,
                    D3D11Av1VideoDecoder::Slot* slot,
                    uint32_t poolEpoch,
                    bool applyGrain)
        : decoder(decoder), slot(slot), poolEpoch(poolEpoch), applyGrain(applyGrain) {}
    D3D11Av1VideoDecoder::Slot* getSlot() const { return slot; }
    bool ApplyGrain() const { return applyGrain; }

    webrtc::scoped_refptr<media::AV1Picture> createDuplicate() override {
        // 与 Chromium D3D11AV1Picture 一致:返回同一对象(引用计数+1),而不是新建一个
        // 共享同一 slot 的新 picture。否则 show_existing_frame 时,重复 picture 一旦析构
        // 就会 returnSlot,把仍被 refFrames_ 引用的槽位提前/重复归还,下一帧复用该槽位
        // 解码即覆盖参考帧 -> 帧间帧解码失败(第一帧正常、第二帧报错)。
        return webrtc::scoped_refptr<media::AV1Picture>(this);
    }

    ~D3D11Av1Picture() override {
        // RAII:槽位在 picture 完全不再被引用(参考帧 + 渲染端都释放)后自动入队回池。
        // 携带创建时的池代次:若期间槽位池已重建/销毁,returnSlot 直接丢弃,
        // 避免把已释放的槽位塞回 freeSlots(悬垂 UAF)。
        if (slot) {
            decoder->returnSlot(slot, poolEpoch);
            slot = nullptr;
        }
    }

private:
    D3D11Av1VideoDecoder* decoder;
    D3D11Av1VideoDecoder::Slot* slot;
    uint32_t poolEpoch;
    bool applyGrain;
};
// 从 Chromium media/gpu/windows/d3d11_av1_accelerator.cc 移植的 FillPicParams:
// 把 libgav1 解析出的序列头/帧头填进 DXVA_PicParams_AV1。
bool FillPicParams(size_t pictureIndex,
                   bool applyGrain,
                   const libgav1::ObuFrameHeader& frameHeader,
                   const libgav1::ObuSequenceHeader& seqHeader,
                   const media::AV1ReferenceFrameVector& refFrames,
                   DXVA_PicParams_AV1* pp) {
    pp->width = frameHeader.width;
    pp->height = frameHeader.height;
    pp->max_width = seqHeader.max_frame_width;
    pp->max_height = seqHeader.max_frame_height;

    pp->CurrPicTextureIndex = pictureIndex;
    pp->superres_denom = frameHeader.use_superres
                             ? frameHeader.superres_scale_denominator
                             : libgav1::kSuperResScaleNumerator;
    pp->bitdepth = seqHeader.color_config.bitdepth;
    pp->seq_profile = seqHeader.profile;

    const auto& tileInfo = frameHeader.tile_info;
    pp->tiles.cols = tileInfo.tile_columns;
    pp->tiles.rows = tileInfo.tile_rows;
    pp->tiles.context_update_id = tileInfo.context_update_id;

    if (tileInfo.uniform_spacing) {
        const auto tileWidthSb =
            (tileInfo.sb_columns + (1 << tileInfo.tile_columns_log2) - 1) >>
            tileInfo.tile_columns_log2;
        const int lastWidthIdx = tileInfo.tile_columns - 1;
        for (int i = 0; i < lastWidthIdx; ++i)
            pp->tiles.widths[i] = tileWidthSb;
        pp->tiles.widths[lastWidthIdx] =
            tileInfo.sb_columns - lastWidthIdx * tileWidthSb;

        const auto tileHeightSb =
            (tileInfo.sb_rows + (1 << tileInfo.tile_rows_log2) - 1) >>
            tileInfo.tile_rows_log2;
        const int lastHeightIdx = tileInfo.tile_rows - 1;
        for (int i = 0; i < lastHeightIdx; ++i)
            pp->tiles.heights[i] = tileHeightSb;
        pp->tiles.heights[lastHeightIdx] =
            tileInfo.sb_rows - lastHeightIdx * tileHeightSb;
    } else {
        for (int i = 0; i < pp->tiles.cols; ++i) {
            pp->tiles.widths[i] =
                frameHeader.tile_info.tile_column_width_in_superblocks[i];
        }
        for (int i = 0; i < pp->tiles.rows; ++i) {
            pp->tiles.heights[i] =
                frameHeader.tile_info.tile_row_height_in_superblocks[i];
        }
    }

    pp->coding.use_128x128_superblock = seqHeader.use_128x128_superblock;
    pp->coding.intra_edge_filter = seqHeader.enable_intra_edge_filter;
    pp->coding.interintra_compound = seqHeader.enable_interintra_compound;
    pp->coding.masked_compound = seqHeader.enable_masked_compound;
    pp->coding.warped_motion = frameHeader.allow_warped_motion;
    pp->coding.dual_filter = seqHeader.enable_dual_filter;
    pp->coding.jnt_comp = seqHeader.enable_jnt_comp;
    pp->coding.screen_content_tools = frameHeader.allow_screen_content_tools;
    pp->coding.integer_mv = frameHeader.force_integer_mv;
    pp->coding.cdef = seqHeader.enable_cdef;
    pp->coding.restoration = seqHeader.enable_restoration;
    pp->coding.film_grain = seqHeader.film_grain_params_present;
    pp->coding.intrabc = frameHeader.allow_intrabc;
    pp->coding.high_precision_mv = frameHeader.allow_high_precision_mv;
    pp->coding.switchable_motion_mode = frameHeader.is_motion_mode_switchable;
    pp->coding.filter_intra = seqHeader.enable_filter_intra;
    pp->coding.disable_frame_end_update_cdf =
        !frameHeader.enable_frame_end_update_cdf;
    pp->coding.disable_cdf_update = !frameHeader.enable_cdf_update;
    pp->coding.reference_mode = frameHeader.reference_mode_select;
    pp->coding.skip_mode = frameHeader.skip_mode_present;
    pp->coding.reduced_tx_set = frameHeader.reduced_tx_set;
    pp->coding.superres = frameHeader.use_superres;
    pp->coding.tx_mode = frameHeader.tx_mode;
    pp->coding.use_ref_frame_mvs = frameHeader.use_ref_frame_mvs;
    pp->coding.enable_ref_frame_mvs = seqHeader.enable_ref_frame_mvs;
    pp->coding.reference_frame_update =
        !(frameHeader.show_existing_frame &&
          frameHeader.frame_type == libgav1::kFrameKey);

    pp->format.frame_type = frameHeader.frame_type;
    pp->format.show_frame = frameHeader.show_frame;
    pp->format.showable_frame = frameHeader.showable_frame;
    pp->format.subsampling_x = seqHeader.color_config.subsampling_x;
    pp->format.subsampling_y = seqHeader.color_config.subsampling_y;
    pp->format.mono_chrome = seqHeader.color_config.is_monochrome;

    pp->primary_ref_frame = frameHeader.primary_reference_frame;
    pp->order_hint = frameHeader.order_hint;
    pp->order_hint_bits = seqHeader.order_hint_bits;

    auto setFrameRefParams = [&](size_t i, size_t refIdx, int32_t width,
                                    int32_t height,
                                    const libgav1::GlobalMotion& gm) {
        pp->frame_refs[i].Index = refIdx;
        pp->frame_refs[i].width = width;
        pp->frame_refs[i].height = height;
        for (size_t j = 0; j < 6; ++j) {
            pp->frame_refs[i].wmmat[j] = gm.params[j];
        }
        pp->frame_refs[i].wmtype = gm.type;
        pp->frame_refs[i].wminvalid =
            gm.type == libgav1::kGlobalMotionTransformationTypeIdentity;
    };

    const bool isIntraFrame = libgav1::IsIntraFrame(frameHeader.frame_type);

    // Chromium d3d11_av1_accelerator 的 disable_invalid_ref 逻辑:某些驱动
    // (尤其 Intel)对非帧内帧的无效参考帧 Index=0xFF 会崩,用第一个有效参考帧顶替。
    int32_t firstValidRefIdx = 0xFF;
    size_t firstValidRefType = 0;
    int32_t firstValidW = 0, firstValidH = 0;
    const D3D11Av1Picture* firstValidRp = nullptr;
    if (!isIntraFrame) {
        for (size_t j = 0; j < libgav1::kNumReferenceFrameTypes - 1; ++j) {
            const auto refIdx = frameHeader.reference_frame_index[j];
            firstValidRp = static_cast<const D3D11Av1Picture*>(refFrames[refIdx].get());
            if (firstValidRp) {
                firstValidRefIdx = refIdx;
                firstValidRefType = j;
                firstValidW = firstValidRp->frameHeader.width;
                firstValidH = firstValidRp->frameHeader.height;
                break;
            }
        }
    }

    for (size_t i = 0; i < libgav1::kNumReferenceFrameTypes - 1; ++i) {
        if (isIntraFrame) {
            pp->frame_refs[i].Index = 0xFF;
            continue;
        }

        const auto refIdx = frameHeader.reference_frame_index[i];
        const auto* rp =
            static_cast<const D3D11Av1Picture*>(refFrames[refIdx].get());

        if (!rp) {
            if (firstValidRefIdx != 0xFF && firstValidRp) {
                // 用第一个有效参考帧顶替 0xFF,规避驱动崩溃。
                const auto& gm = firstValidRp->frameHeader.global_motion[
                    libgav1::kReferenceFrameLast + firstValidRefType];
                setFrameRefParams(i, firstValidRefIdx, firstValidW, firstValidH, gm);
            } else {
                pp->frame_refs[i].Index = 0xFF;
            }
        } else {
            const auto& gm =
                frameHeader.global_motion[libgav1::kReferenceFrameLast + i];
            setFrameRefParams(i, refIdx, rp->frameHeader.width,
                                 rp->frameHeader.height, gm);
        }
    }

    for (size_t i = 0; i < libgav1::kNumReferenceFrameTypes; ++i) {
        const auto* rp =
            static_cast<const D3D11Av1Picture*>(refFrames[i].get());
        pp->RefFrameMapTextureIndex[i] =
            rp ? rp->getSlot()->index : 0xFF;
    }

    pp->loop_filter.filter_level[0] = frameHeader.loop_filter.level[0];
    pp->loop_filter.filter_level[1] = frameHeader.loop_filter.level[1];
    pp->loop_filter.filter_level_u = frameHeader.loop_filter.level[2];
    pp->loop_filter.filter_level_v = frameHeader.loop_filter.level[3];
    pp->loop_filter.sharpness_level = frameHeader.loop_filter.sharpness;
    pp->loop_filter.mode_ref_delta_enabled =
        frameHeader.loop_filter.delta_enabled;
    pp->loop_filter.mode_ref_delta_update = frameHeader.loop_filter.delta_update;
    pp->loop_filter.delta_lf_multi = frameHeader.delta_lf.multi;
    pp->loop_filter.delta_lf_present = frameHeader.delta_lf.present;

    for (size_t i = 0; i < libgav1::kNumReferenceFrameTypes; ++i)
        pp->loop_filter.ref_deltas[i] = frameHeader.loop_filter.ref_deltas[i];
    pp->loop_filter.mode_deltas[0] = frameHeader.loop_filter.mode_deltas[0];
    pp->loop_filter.mode_deltas[1] = frameHeader.loop_filter.mode_deltas[1];
    pp->loop_filter.delta_lf_res = frameHeader.delta_lf.scale;

    for (size_t i = 0; i < libgav1::kMaxPlanes; ++i) {
        constexpr uint8_t kD3D11LoopRestorationMapping[4] = {
            0,  // libgav1::kLoopRestorationTypeNone,
            3,  // libgav1::kLoopRestorationTypeSwitchable,
            1,  // libgav1::kLoopRestorationTypeWiener,
            2,  // libgav1::kLoopRestorationTypeSgrProj
        };
        pp->loop_filter.frame_restoration_type[i] =
            kD3D11LoopRestorationMapping[frameHeader.loop_restoration.type[i]];
        pp->loop_filter.log2_restoration_unit_size[i] =
            frameHeader.loop_restoration.unit_size_log2[i];
    }

    pp->quantization.delta_q_present = frameHeader.delta_q.present;
    pp->quantization.delta_q_res = frameHeader.delta_q.scale;
    pp->quantization.base_qindex = frameHeader.quantizer.base_index;
    pp->quantization.y_dc_delta_q = frameHeader.quantizer.delta_dc[0];
    pp->quantization.u_dc_delta_q = frameHeader.quantizer.delta_dc[1];
    pp->quantization.v_dc_delta_q = frameHeader.quantizer.delta_dc[2];
    pp->quantization.u_ac_delta_q = frameHeader.quantizer.delta_ac[1];
    pp->quantization.v_ac_delta_q = frameHeader.quantizer.delta_ac[2];
    pp->quantization.qm_y = frameHeader.quantizer.use_matrix
                                ? frameHeader.quantizer.matrix_level[0]
                                : 0xFF;
    pp->quantization.qm_u = frameHeader.quantizer.use_matrix
                                ? frameHeader.quantizer.matrix_level[1]
                                : 0xFF;
    pp->quantization.qm_v = frameHeader.quantizer.use_matrix
                                ? frameHeader.quantizer.matrix_level[2]
                                : 0xFF;

    const uint8_t coeffShift = pp->bitdepth - 8;
    pp->cdef.damping = frameHeader.cdef.damping - coeffShift - 3u;
    pp->cdef.bits = frameHeader.cdef.bits;
    for (size_t i = 0; i < libgav1::kMaxCdefStrengths; ++i) {
        uint8_t yStr = frameHeader.cdef.y_secondary_strength[i] >> coeffShift;
        uint8_t uvStr = frameHeader.cdef.uv_secondary_strength[i] >> coeffShift;
        yStr = yStr == 4 ? 3 : yStr;
        uvStr = uvStr == 4 ? 3 : uvStr;
        pp->cdef.y_strengths[i].primary =
            frameHeader.cdef.y_primary_strength[i] >> coeffShift;
        pp->cdef.y_strengths[i].secondary = yStr;
        pp->cdef.uv_strengths[i].primary =
            frameHeader.cdef.uv_primary_strength[i] >> coeffShift;
        pp->cdef.uv_strengths[i].secondary = uvStr;
    }

    pp->interp_filter = frameHeader.interpolation_filter;

    pp->segmentation.enabled = frameHeader.segmentation.enabled;
    pp->segmentation.update_map = frameHeader.segmentation.update_map;
    pp->segmentation.update_data = frameHeader.segmentation.update_data;
    pp->segmentation.temporal_update = frameHeader.segmentation.temporal_update;
    for (size_t i = 0; i < libgav1::kMaxSegments; ++i) {
        for (size_t j = 0; j < libgav1::kSegmentFeatureMax; ++j) {
            pp->segmentation.feature_mask[i].mask |=
                frameHeader.segmentation.feature_enabled[i][j] << j;
            pp->segmentation.feature_data[i][j] =
                frameHeader.segmentation.feature_data[i][j];
        }
    }

    // Film grain:libgav1 提供的是"减去 128/256 后"的值,DXVA 规范要求原始值,需加回。
    // (Chromium d3d11_av1_accelerator 同款逻辑。屏录一般不用 grain,但驱动拿到
    // 全零 grain 参数可能行为异常,补齐更稳。)
    if (applyGrain) {
        const auto& fg = frameHeader.film_grain_params;
        pp->film_grain.apply_grain = fg.apply_grain;
        pp->film_grain.scaling_shift_minus8 = fg.chroma_scaling - 8;
        pp->film_grain.chroma_scaling_from_luma = fg.chroma_scaling_from_luma;
        pp->film_grain.ar_coeff_lag = fg.auto_regression_coeff_lag;
        pp->film_grain.ar_coeff_shift_minus6 = fg.auto_regression_shift - 6;
        pp->film_grain.grain_scale_shift = fg.grain_scale_shift;
        pp->film_grain.overlap_flag = fg.overlap_flag;
        pp->film_grain.clip_to_restricted_range = fg.clip_to_restricted_range;
        pp->film_grain.matrix_coeff_is_identity =
            seqHeader.color_config.matrix_coefficients ==
            libgav1::kMatrixCoefficientsIdentity;
        pp->film_grain.grain_seed = fg.grain_seed;
        pp->film_grain.num_y_points = fg.num_y_points;
        for (uint8_t i = 0; i < fg.num_y_points; ++i) {
            pp->film_grain.scaling_points_y[i][0] = fg.point_y_value[i];
            pp->film_grain.scaling_points_y[i][1] = fg.point_y_scaling[i];
        }
        pp->film_grain.num_cb_points = fg.num_u_points;
        for (uint8_t i = 0; i < fg.num_u_points; ++i) {
            pp->film_grain.scaling_points_cb[i][0] = fg.point_u_value[i];
            pp->film_grain.scaling_points_cb[i][1] = fg.point_u_scaling[i];
        }
        pp->film_grain.num_cr_points = fg.num_v_points;
        for (uint8_t i = 0; i < fg.num_v_points; ++i) {
            pp->film_grain.scaling_points_cr[i][0] = fg.point_v_value[i];
            pp->film_grain.scaling_points_cr[i][1] = fg.point_v_scaling[i];
        }
        for (size_t i = 0; i < 24; ++i) {
            pp->film_grain.ar_coeffs_y[i] = fg.auto_regression_coeff_y[i] + 128;
        }
        for (size_t i = 0; i < 25; ++i) {
            pp->film_grain.ar_coeffs_cb[i] = fg.auto_regression_coeff_u[i] + 128;
            pp->film_grain.ar_coeffs_cr[i] = fg.auto_regression_coeff_v[i] + 128;
        }
        if (fg.num_u_points > 0) {
            pp->film_grain.cb_mult = fg.u_multiplier + 128;
            pp->film_grain.cb_luma_mult = fg.u_luma_multiplier + 128;
            pp->film_grain.cb_offset = fg.u_offset + 256;
        }
        if (fg.num_v_points > 0) {
            pp->film_grain.cr_mult = fg.v_multiplier + 128;
            pp->film_grain.cr_luma_mult = fg.v_luma_multiplier + 128;
            pp->film_grain.cr_offset = fg.v_offset + 256;
        }
    }

    return true;
}

// 从 Chromium media/gpu/windows/d3d11_av1_accelerator.cc 移植的 submitDecoderBuffer:
// 填 DXVA_Tile_AV1 并把 tile 数据拷进 bitstream buffer。
}  // namespace

// =============================================================================
//  AV1 accelerator:实现 media::AV1Decoder::AV1Accelerator,直接驱动 D3D11。
// =============================================================================
class D3D11Av1Accelerator : public media::AV1Decoder::AV1Accelerator {
public:
    explicit D3D11Av1Accelerator(D3D11Av1VideoDecoder* decoder) : decoder(decoder) {}

    webrtc::scoped_refptr<media::AV1Picture> createAV1Picture(bool applyGrain) override {
        auto* slot = decoder->acquireFreeSlot();
        if (!slot) {
            LOG_ERROR("[D3D11Av1VideoDecoder] slot pool exhausted (all %d slots held by DPB/delivered frames)",
                      decoder->slotPoolSize());
            return nullptr;
        }
        return base::MakeRefCounted<D3D11Av1Picture>(
            decoder, slot, decoder->poolEpoch.load(std::memory_order_acquire), applyGrain);
    }

    Status submitDecode(const media::AV1Picture& pic,
                        const libgav1::ObuSequenceHeader& seqHeader,
                        const media::AV1ReferenceFrameVector& refFrames,
                        const libgav1::Vector<libgav1::TileBuffer>& tileBuffers,
                        base::span<const uint8_t> /*data*/) override {
        const auto* d3d11Pic = static_cast<const D3D11Av1Picture*>(&pic);
        auto* slot = d3d11Pic->getSlot();
        if (!slot || !decoder->videoDecoder || !decoder->videoContext) {
            LOG_ERROR("[D3D11Av1VideoDecoder] submitDecode guard failed: slot=%d videoDecoder=%d videoContext=%d",
                      slot != nullptr, decoder->videoDecoder != nullptr, decoder->videoContext != nullptr);
            return Status::kFail;
        }

        DXVA_PicParams_AV1 picParams{};
        if (!FillPicParams(slot->index, d3d11Pic->ApplyGrain(), pic.frameHeader,
                           seqHeader, refFrames, &picParams)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] FillPicParams failed");
            return Status::kFail;
        }

        // 1. DecoderBeginFrame。硬件忙时(上一帧仍在解码)返回 E_PENDING /
        //    D3DERR_WASSTILLDRAWING,须让出线程后重试(Chromium d3d11_video_decoderwrapper
        //    ::WaitForFrameBegins 同款逻辑)。第一帧时解码器空闲,后续帧紧跟会命中忙状态。
        HRESULT hr = E_FAIL;
        int beginRetries = 0;
        constexpr int kMaxBeginFrameRetries = 1000;
        // 解码输出视图:ZeroCopy 用共享纹理,Copy 用私有解码纹理(拷后再到共享纹理)。
        ID3D11VideoDecoderOutputView* decodeOutView =
            decoder->decodePath == D3D11Av1VideoDecoder::DecodePath::Copy
                ? slot->decodeOutputView.Get()
                : slot->outputView.Get();
        do {
            hr = decoder->videoContext->DecoderBeginFrame(
                decoder->videoDecoder.Get(), decodeOutView, 0, nullptr);
            if (hr == E_PENDING || hr == kD3DErrWasStillDrawing) {
                std::this_thread::yield();
                if (++beginRetries >= kMaxBeginFrameRetries) {
                    LOG_ERROR("[D3D11Av1VideoDecoder] DecoderBeginFrame still busy after %d retries", beginRetries);
                    break;
                }
            }
        } while (hr == E_PENDING || hr == kD3DErrWasStillDrawing);
        if (FAILED(hr)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] DecoderBeginFrame failed hr=0x%08X (retries=%d)", (unsigned)hr, beginRetries);
            return Status::kFail;
        }
        if (beginRetries > 0) {
            LOG_INFO("[D3D11Av1VideoDecoder] DecoderBeginFrame retried %d times", beginRetries);
        }

        // 2. 图片参数
        UINT bufSize = 0;
        void* buf = nullptr;
        hr = decoder->videoContext->GetDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &bufSize, &buf);
        if (FAILED(hr) || bufSize < sizeof(picParams)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] GetDecoderBuffer(PIC) failed hr=0x%08X", (unsigned)hr);
            return Status::kFail;
        }
        memcpy(buf, &picParams, sizeof(picParams));
        decoder->videoContext->ReleaseDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);

        // 3. 切片控制(tile 列表)
        const size_t tileCount = tileBuffers.size();
        hr = decoder->videoContext->GetDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &bufSize, &buf);
        if (FAILED(hr) || bufSize < tileCount * sizeof(DXVA_Tile_AV1)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] GetDecoderBuffer(SLICE) failed hr=0x%08X", (unsigned)hr);
            return Status::kFail;
        }
        auto* tiles = static_cast<DXVA_Tile_AV1*>(buf);
        size_t tileOffset = 0;
        for (size_t i = 0; i < tileCount; ++i) {
            tiles[i].DataOffset = tileOffset;
            tiles[i].DataSize = tileBuffers[i].size;
            tiles[i].row = i / picParams.tiles.cols;
            tiles[i].column = i % picParams.tiles.cols;
            tiles[i].anchor_frame = 0xFF;
            tileOffset += tileBuffers[i].size;
        }
        decoder->videoContext->ReleaseDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);

        // 4. 码流(tile 数据)
        hr = decoder->videoContext->GetDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bufSize, &buf);
        if (FAILED(hr) || bufSize < tileOffset) {
            LOG_ERROR("[D3D11Av1VideoDecoder] GetDecoderBuffer(BITSTREAM) failed hr=0x%08X", (unsigned)hr);
            return Status::kFail;
        }
        auto* dst = static_cast<uint8_t*>(buf);
        for (size_t i = 0; i < tileCount; ++i) {
            memcpy(dst, tileBuffers[i].data, tileBuffers[i].size);
            dst += tileBuffers[i].size;
        }
        decoder->videoContext->ReleaseDecoderBuffer(
            decoder->videoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);

        // 5. 提交 + 结束帧
        D3D11_VIDEO_DECODER_BUFFER_DESC desc[3] = {};
        desc[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
        desc[0].DataSize = sizeof(picParams);
        desc[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        desc[1].DataSize = tileCount * sizeof(DXVA_Tile_AV1);
        desc[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        desc[2].DataSize = tileOffset;
        hr = decoder->videoContext->SubmitDecoderBuffers(
            decoder->videoDecoder.Get(), 3, desc);
        if (FAILED(hr)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] SubmitDecoderBuffers failed hr=0x%08X", (unsigned)hr);
            return Status::kFail;
        }
        hr = decoder->videoContext->DecoderEndFrame(decoder->videoDecoder.Get());
        if (FAILED(hr)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] DecoderEndFrame failed hr=0x%08X", (unsigned)hr);
            return Status::kFail;
        }
        // Copy 路径:VideoProcessorBlt 把私有解码纹理拷到共享输出纹理。
        // 参考 Chromium d3d11_copying_texture_wrapper::ProcessTexture。
        if (decoder->decodePath == D3D11Av1VideoDecoder::DecodePath::Copy) {
            if (!decoder->videoProcessor || !slot->vpInputView || !slot->vpOutputView) {
                LOG_ERROR("[D3D11Av1VideoDecoder] copy path: videoProcessor/views not ready");
                return Status::kFail;
            }
            D3D11_VIDEO_PROCESSOR_STREAM streams{};
            streams.Enable = TRUE;
            streams.pInputSurface = slot->vpInputView.Get();
            hr = decoder->videoContext->VideoProcessorBlt(
                decoder->videoProcessor.Get(), slot->vpOutputView.Get(), 0, 1, &streams);
            if (FAILED(hr)) {
                LOG_ERROR("[D3D11Av1VideoDecoder] VideoProcessorBlt failed hr=0x%08X", (unsigned)hr);
                return Status::kFail;
            }
        }
        // 解码完成查询:outputFrame 交付前自旋等待,保证渲染端读到完整帧(含 VP 拷贝)。
        if (decoder->copyQuery) decoder->decodeContext->End(decoder->copyQuery.Get());
        return Status::kOk;
    }

    bool outputPicture(const media::AV1Picture& pic) override {
        const auto* d3d11Pic = static_cast<const D3D11Av1Picture*>(&pic);
        bool ok = decoder->outputFrame(d3d11Pic->getSlot(), pic);
        if (!ok) {
            LOG_ERROR("[D3D11Av1VideoDecoder] outputPicture failed slot=%d show_frame=%d",
                      d3d11Pic->getSlot() ? d3d11Pic->getSlot()->index : -1, (int)pic.frameHeader.show_frame);
        }
        return ok;
    }

private:
    D3D11Av1VideoDecoder* decoder;
};

// =============================================================================
//  D3D11Av1VideoDecoder
// =============================================================================
D3D11Av1VideoDecoder::D3D11Av1VideoDecoder() = default;

D3D11Av1VideoDecoder::~D3D11Av1VideoDecoder() {
    if (ownerFactory) ownerFactory->removeDecoder(this);
    Release();
}

bool D3D11Av1VideoDecoder::Configure(const Settings& /*settings*/) {
    std::lock_guard<std::mutex> lock(mutex);
    released.store(false, std::memory_order_release);  // 复用解码器时重置释放标志
    if (initialized) return true;
    if (!ensureInitialized()) {
        LOG_ERROR("[D3D11Av1VideoDecoder] init failed");
        return false;
    }
    return true;
}

bool D3D11Av1VideoDecoder::ensureInitialized() {
    if (initialized) return true;
    // 渲染设备没注入前不建解码设备:避免用 D3D_DRIVER_TYPE_HARDWARE 建到错误的适配器。
    // 等 setD3D11Device 注入当前连接的渲染设备后再初始化。
    if (!renderDevice) {
        LOG_INFO("[D3D11Av1VideoDecoder] ensureInitialized deferred: no render device yet");
        return true;
    }
    if (!createDecodeDevice()) return false;
    // 默认零拷贝(单共享纹理直解码);驱动不认共享解码纹理则改用拷贝路径
    // (解码进私有纹理 + VideoProcessor 拷到共享纹理),而不是整体失败回软解。
    if (!probeSingleTextureSupport()) {
        LOG_WARN("[D3D11Av1VideoDecoder] single shared texture not supported, switch to COPY path");
        decodePath = DecodePath::Copy;
    }
    initialized = true;
    LOG_INFO("[D3D11Av1VideoDecoder] decode device ready (DXVA AV1, path=%s)",
             decodePath == DecodePath::ZeroCopy ? "zero-copy" : "copy");
    return true;
}

bool D3D11Av1VideoDecoder::createDecodeDevice() {
    if (decodeDevice) return true;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (renderDevice) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(renderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
            dxgiDev->GetAdapter(&adapter);
        }
    }

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        &decodeDevice, &fl, &decodeContext);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] D3D11CreateDevice failed hr=0x%08X", (unsigned)hr);
        decodeDevice.Reset();
        decodeContext.Reset();
        return false;
    }
    if (FAILED(decodeDevice.As(&videoDevice)) ||
        FAILED(decodeContext.As(&videoContext))) {
        LOG_ERROR("[D3D11Av1VideoDecoder] QI video device/context failed");
        decodeDevice.Reset();
        decodeContext.Reset();
        return false;
    }
    // 拷贝完成查询:decTex -> sharedTex 拷贝在 GPU 上完成后,渲染端才读共享纹理。
    // 查询在 decodeContext,拷贝也在这,End 后 GetData 等到拷贝(及之前的解码)完成。
    D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
    if (FAILED(decodeDevice->CreateQuery(&qd, &copyQuery))) {
        LOG_WARN("[D3D11Av1VideoDecoder] CreateQuery(copyQuery) failed, copy sync disabled");
    }
    return true;
}

bool D3D11Av1VideoDecoder::recreateDecoder(int width, int height,
                                           media::VideoCodecProfile profile) {
    currentProfile = profile;
    if (videoDecoder && codedWidth == width && codedHeight == height) return true;
    videoDecoder.Reset();
    destroySlots();

    // 拷贝路径:解码进私有纹理后要 VideoProcessor 拷到共享纹理,先建 VP。
    if (decodePath == DecodePath::Copy && !ensureVideoProcessor(width, height)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] ensureVideoProcessor failed %dx%d", width, height);
        return false;
    }

    GUID decoderGuid = DXVA_ModeAV1_VLD_Profile0;
    if (profile == media::AV1PROFILE_PROFILE_HIGH)
        decoderGuid = DXVA_ModeAV1_VLD_Profile1;

    BOOL supported = FALSE;
    HRESULT hr = videoDevice->CheckVideoDecoderFormat(&decoderGuid, DXGI_FORMAT_NV12, &supported);
    if (FAILED(hr) || !supported) {
        LOG_ERROR("[D3D11Av1VideoDecoder] AV1 decoder format not supported (hr=0x%08X supported=%d)", (unsigned)hr, supported);
        return false;
    }

    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = decoderGuid;
    desc.SampleWidth = (UINT)width;
    desc.SampleHeight = (UINT)height;
    desc.OutputFormat = DXGI_FORMAT_NV12;

    // AV1(DXVA)规范要求 ConfigBitstreamRaw == 1,不能传 nullptr 的 decoder config。
    UINT configCount = 0;
    hr = videoDevice->GetVideoDecoderConfigCount(&desc, &configCount);
    if (FAILED(hr) || configCount == 0) {
        LOG_ERROR("[D3D11Av1VideoDecoder] GetVideoDecoderConfigCount failed hr=0x%08X count=%u", (unsigned)hr, configCount);
        return false;
    }
    D3D11_VIDEO_DECODER_CONFIG decConfig{};
    bool configFound = false;
    for (UINT i = 0; i < configCount; ++i) {
        hr = videoDevice->GetVideoDecoderConfig(&desc, i, &decConfig);
        if (FAILED(hr)) continue;
        if (decConfig.ConfigBitstreamRaw == 1) {  // AV1 规范要求
            configFound = true;
            break;
        }
    }
    if (!configFound) {
        LOG_ERROR("[D3D11Av1VideoDecoder] no decoder config with ConfigBitstreamRaw=1");
        return false;
    }

    hr = videoDevice->CreateVideoDecoder(&desc, &decConfig, &videoDecoder);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoDecoder failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    codedWidth = width;
    codedHeight = height;
    LOG_INFO("[D3D11Av1VideoDecoder] AV1 decoder created %dx%d", width, height);

    if (!ensureSlots(width, height, decoderGuid)) return false;
    return true;
}

// 零拷贝解码失败(解码错误/GPU 设备移除)时,销毁重建为拷贝路径。
// 参考 Chromium:解码进私有纹理 -> VideoProcessorBlt 拷到共享输出纹理,
// 绕开"解码直写共享纹理"这一驱动不保证支持、容易崩 GPU 的路径。
bool D3D11Av1VideoDecoder::switchToCopyMode() {
    if (decodePath != DecodePath::ZeroCopy) return true;
    LOG_WARN("[D3D11Av1VideoDecoder] switchToCopyMode: zero-copy -> COPY path (VideoProcessor)");

    // 销毁当前解码器/槽位(可能 GPU 已移除)。
    videoDecoder.Reset();
    destroySlots();
    videoProcessor.Reset();
    vpEnumerator.Reset();
    vpWidth = vpHeight = 0;

    // 设备移除(TDR)后解码设备也要重建;渲染设备由上层(QRhi)重建。
    HRESULT removedReason = decodeDevice ? decodeDevice->GetDeviceRemovedReason() : S_OK;
    if (removedReason != S_OK) {
        LOG_WARN("[D3D11Av1VideoDecoder] decode device removed (hr=0x%08X), recreating", (unsigned)removedReason);
        decodeContext.Reset();
        videoDevice.Reset();
        videoContext.Reset();
        decodeDevice.Reset();
        if (!createDecodeDevice()) {
            LOG_ERROR("[D3D11Av1VideoDecoder] recreate decode device failed");
            return false;
        }
    }

    int w = codedWidth, h = codedHeight;
    if (w <= 0 || h <= 0) {
        const gfx::Size s = av1Decoder ? av1Decoder->GetPicSize() : gfx::Size();
        w = s.width();
        h = s.height();
    }
    if (w <= 0 || h <= 0) {
        LOG_ERROR("[D3D11Av1VideoDecoder] no known size for copy mode, cannot recreate");
        return false;
    }
    decodePath = DecodePath::Copy;
    if (!recreateDecoder(w, h, currentProfile)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] recreateDecoder(copy) failed");
        return false;
    }
    LOG_INFO("[D3D11Av1VideoDecoder] 已进入拷贝模式(解码进私有纹理 -> VideoProcessor 拷贝到共享纹理)");
    return true;
}

bool D3D11Av1VideoDecoder::ensureSlots(int width, int height, const GUID& decoderGuid) {

    static constexpr int kSlotCount = 48;

    std::lock_guard<std::mutex> lock(poolMutex);
    for (int i = 0; i < kSlotCount; ++i) {
        if ((int)slots.size() <= i)
            slots.push_back(std::make_unique<Slot>());
        if (!createSlotOne(*slots[i], i, width, height, decoderGuid)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] slot pool creation failed");
            return false;
        }
    }
    for (auto& up : slots) freeSlots.enqueue(up.get());
    slotCount.store(kSlotCount, std::memory_order_relaxed);
    LOG_INFO("[D3D11Av1VideoDecoder] slot pool ready %dx%d x%d", width, height, kSlotCount);
    return true;
}

bool D3D11Av1VideoDecoder::ensureVideoProcessor(int width, int height) {
    if (videoProcessor && vpWidth == width && vpHeight == height)
        return true;
    videoProcessor.Reset();
    vpEnumerator.Reset();
    vpWidth = vpHeight = 0;

    // 内容描述里的尺寸只用于初始化,不影响实际拷贝;这里用解码输出尺寸。
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = 60;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = (UINT)width;
    desc.InputHeight = (UINT)height;
    desc.OutputFrameRate.Numerator = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth = (UINT)width;
    desc.OutputHeight = (UINT)height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice->CreateVideoProcessorEnumerator(&desc, &vpEnumerator);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoProcessorEnumerator failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    hr = videoDevice->CreateVideoProcessor(vpEnumerator.Get(), 0, &videoProcessor);
    if (FAILED(hr)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoProcessor failed hr=0x%08X", (unsigned)hr);
        vpEnumerator.Reset();
        return false;
    }
    vpWidth = width;
    vpHeight = height;
    LOG_INFO("[D3D11Av1VideoDecoder] VideoProcessor created %dx%d (copy path)", width, height);
    return true;
}

bool D3D11Av1VideoDecoder::createSlotOne(Slot& slot, int index, int width, int height,
                                         const GUID& decoderGuid) {
    // 池复用/重建:先释放旧资源,避免残留上一代纹理/视图。
    slot.texture.Reset();
    slot.outputView.Reset();
    slot.decodeTexture.Reset();
    slot.decodeOutputView.Reset();
    slot.sharedTexture.Reset();
    slot.vpInputView.Reset();
    slot.vpOutputView.Reset();
    slot.renderTexture.Reset();
    slot.planeYSrv.Reset();
    slot.planeUvSrv.Reset();
    slot.renderDeviceCached = nullptr;

    D3D11_TEXTURE2D_DESC baseTextureDesc{};
    baseTextureDesc.Width = (UINT)width;
    baseTextureDesc.Height = (UINT)height;
    baseTextureDesc.MipLevels = 1;
    baseTextureDesc.ArraySize = 1;
    baseTextureDesc.Format = DXGI_FORMAT_NV12;
    baseTextureDesc.SampleDesc.Count = 1;
    baseTextureDesc.Usage = D3D11_USAGE_DEFAULT;

    if (decodePath == DecodePath::ZeroCopy) {
        // 单共享纹理直解码(BIND_DECODER|SHADER_RESOURCE + SHARED_NTHANDLE)。
        D3D11_TEXTURE2D_DESC singleTextureDesc = baseTextureDesc;
        singleTextureDesc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        singleTextureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hrResult = decodeDevice->CreateTexture2D(&singleTextureDesc, nullptr, &slot.texture);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateTexture2D(single) failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }

        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC outputViewDesc{};
        outputViewDesc.DecodeProfile = decoderGuid;
        outputViewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        outputViewDesc.Texture2D.ArraySlice = 0;
        hrResult = videoDevice->CreateVideoDecoderOutputView(slot.texture.Get(), &outputViewDesc, &slot.outputView);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoDecoderOutputView failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }
    } else {
        // Copy 路径:私有解码纹理(不可共享) + 共享输出纹理,VideoProcessor 拷贝。
        D3D11_TEXTURE2D_DESC decodeTexDesc = baseTextureDesc;
        decodeTexDesc.BindFlags = D3D11_BIND_DECODER;
        decodeTexDesc.MiscFlags = 0;
        HRESULT hrResult = decodeDevice->CreateTexture2D(&decodeTexDesc, nullptr, &slot.decodeTexture);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateTexture2D(decode/private) failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC decodeOutViewDesc{};
        decodeOutViewDesc.DecodeProfile = decoderGuid;
        decodeOutViewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        decodeOutViewDesc.Texture2D.ArraySlice = 0;
        hrResult = videoDevice->CreateVideoDecoderOutputView(slot.decodeTexture.Get(), &decodeOutViewDesc, &slot.decodeOutputView);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoDecoderOutputView(decode/private) failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }

        // 共享输出纹理(BIND_RT|SRV + SHARED_NTHANDLE),VideoProcessorBlt 的拷贝目标。
        D3D11_TEXTURE2D_DESC sharedTexDesc = baseTextureDesc;
        sharedTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        sharedTexDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        hrResult = decodeDevice->CreateTexture2D(&sharedTexDesc, nullptr, &slot.sharedTexture);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateTexture2D(shared/out) failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }

        // VideoProcessor 输入视图(私有解码纹理) + 输出视图(共享纹理)。
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vpivDesc{};
        vpivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        vpivDesc.Texture2D.ArraySlice = 0;
        vpivDesc.Texture2D.MipSlice = 0;
        hrResult = videoDevice->CreateVideoProcessorInputView(
            slot.decodeTexture.Get(), vpEnumerator.Get(), &vpivDesc, &slot.vpInputView);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoProcessorInputView failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vpovDesc{};
        vpovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        vpovDesc.Texture2D.MipSlice = 0;
        hrResult = videoDevice->CreateVideoProcessorOutputView(
            slot.sharedTexture.Get(), vpEnumerator.Get(), &vpovDesc, &slot.vpOutputView);
        if (FAILED(hrResult)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] CreateVideoProcessorOutputView failed hr=0x%08X", (unsigned)hrResult);
            return false;
        }
    }
    slot.index = index;
    return true;
}

bool D3D11Av1VideoDecoder::probeSingleTextureSupport() {
    if (!decodeDevice) return false;
    // 探针:能否创建 BIND_DECODER|SHADER_RESOURCE + SHARED_NTHANDLE 纹理并可共享。
    D3D11_TEXTURE2D_DESC probeDesc{};
    probeDesc.Width = 640;
    probeDesc.Height = 480;
    probeDesc.MipLevels = 1;
    probeDesc.ArraySize = 1;
    probeDesc.Format = DXGI_FORMAT_NV12;
    probeDesc.SampleDesc.Count = 1;
    probeDesc.Usage = D3D11_USAGE_DEFAULT;
    probeDesc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    probeDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> probeTexture;
    if (FAILED(decodeDevice->CreateTexture2D(&probeDesc, nullptr, &probeTexture)))
        return false;
    Microsoft::WRL::ComPtr<IDXGIResource1> probeResource;
    HANDLE probeHandle = nullptr;
    bool supported = SUCCEEDED(probeTexture.As(&probeResource)) &&
                     SUCCEEDED(probeResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &probeHandle));
    if (probeHandle) CloseHandle(probeHandle);
    return supported;
}

bool D3D11Av1VideoDecoder::openSharedTexForRender(
    ID3D11Texture2D* sharedTexture,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& renderTexture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1>& planeYSrv,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1>& planeUvSrv) {
    // NT handle 优先,回退 legacy GetSharedHandle。
    HANDLE sharedHandle = nullptr;
    bool haveSharedHandle = false;
    Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource1;
    if (SUCCEEDED(sharedTexture->QueryInterface(IID_PPV_ARGS(&dxgiResource1)))) {
        if (SUCCEEDED(dxgiResource1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle)))
            haveSharedHandle = true;
    }
    if (!haveSharedHandle) {
        Microsoft::WRL::ComPtr<IDXGIResource> dxgiResource;
        if (SUCCEEDED(sharedTexture->QueryInterface(IID_PPV_ARGS(&dxgiResource)))) {
            if (SUCCEEDED(dxgiResource->GetSharedHandle(&sharedHandle))) haveSharedHandle = true;
        }
    }
    if (!haveSharedHandle) {
        LOG_ERROR("[D3D11Av1VideoDecoder] texture not shareable");
        return false;
    }

    // 打开到渲染设备:OpenSharedResource1,回退 OpenSharedResource。
    HRESULT hrResult = E_FAIL;
    Microsoft::WRL::ComPtr<ID3D11Device1> d3d11Device1;
    if (SUCCEEDED(renderDevice.As(&d3d11Device1))) {
        hrResult = d3d11Device1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&renderTexture));
    }
    if (FAILED(hrResult)) {
        hrResult = renderDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&renderTexture));
    }
    CloseHandle(sharedHandle);
    if (FAILED(hrResult)) {
        LOG_ERROR("[D3D11Av1VideoDecoder] OpenSharedResource failed hr=0x%08X", (unsigned)hrResult);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device3> d3d11Device3;
    if (FAILED(renderDevice.As(&d3d11Device3))) {
        LOG_ERROR("[D3D11Av1VideoDecoder] renderDevice QI ID3D11Device3 failed");
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    if (FAILED(d3d11Device3->CreateShaderResourceView1(renderTexture.Get(), &srvDesc, planeYSrv.GetAddressOf()))) {
        LOG_ERROR("[D3D11Av1VideoDecoder] CreateShaderResourceView1(Y plane) failed");
        return false;
    }
    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDesc.Texture2D.PlaneSlice = 1;
    if (FAILED(d3d11Device3->CreateShaderResourceView1(renderTexture.Get(), &srvDesc, planeUvSrv.GetAddressOf()))) {
        LOG_ERROR("[D3D11Av1VideoDecoder] CreateShaderResourceView1(UV plane) failed");
        return false;
    }
    return true;
}

D3D11Av1VideoDecoder::Slot* D3D11Av1VideoDecoder::acquireFreeSlot() {
    std::lock_guard<std::mutex> lock(poolMutex);
    Slot* s = nullptr;
    return freeSlots.try_dequeue(s) ? s : nullptr;
}

void D3D11Av1VideoDecoder::returnSlot(Slot* s, uint32_t epoch) {
    std::lock_guard<std::mutex> lock(poolMutex);
    // epoch 不匹配 = 该槽位所属的池已重建/销毁,直接丢弃(不能解引用 s)。
    if (!s || epoch != poolEpoch.load(std::memory_order_acquire)) return;
    freeSlots.enqueue(s);
}

int D3D11Av1VideoDecoder::slotPoolSize() {
    return slotCount.load(std::memory_order_relaxed);
}

void D3D11Av1VideoDecoder::destroySlots() {
    std::lock_guard<std::mutex> lock(poolMutex);
    // 池代次+1:已创建的 picture(参考帧/渲染队列里的帧)稍后析构 returnSlot 时
    // epoch 不匹配 -> 直接丢弃,不会向 freeSlots 塞已释放的槽位(悬垂 UAF)。
    poolEpoch.fetch_add(1, std::memory_order_acq_rel);
    Slot* s;
    while (freeSlots.try_dequeue(s)) {}
    slotCount.store(0, std::memory_order_relaxed);
    // 释放纹理/视图。unique_ptr 槽位对象保留:旧 picture 的 returnSlot 用 epoch 判断,
    // 不持有旧槽位对象的引用,因此这里释放是安全的。渲染队列里的帧持有自己的
    // planeY/UvSrv(ComPtr 拷贝),不受影响。
    for (auto& up : slots) {
        if (!up) continue;
        up->texture.Reset();
        up->outputView.Reset();
        up->decodeTexture.Reset();
        up->decodeOutputView.Reset();
        up->sharedTexture.Reset();
        up->vpInputView.Reset();
        up->vpOutputView.Reset();
        up->renderTexture.Reset();
        up->planeYSrv.Reset();
        up->planeUvSrv.Reset();
        up->renderDeviceCached = nullptr;
    }
}

int32_t D3D11Av1VideoDecoder::Decode(const webrtc::EncodedImage& inputImage,
                                     bool /*missingFrames*/,
                                     int64_t renderTimeMs) {

    std::unique_lock<std::mutex> lock(mutex);

    renderCv.wait(lock, [&] { return renderDevice != nullptr || released.load(std::memory_order_acquire); });
    if (!renderDevice) {
        LOG_WARN("[D3D11Av1VideoDecoder] Decode after release");
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (!decodeCallback) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (!initialized && !ensureInitialized()) {
        LOG_ERROR("[D3D11Av1VideoDecoder] not initialized");
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (!av1Decoder) {
        av1Decoder = std::make_unique<media::AV1Decoder>(
            std::make_unique<D3D11Av1Accelerator>(this),
            media::AV1PROFILE_PROFILE_MAIN);
    }

    metaQueue.push_back({ inputImage.RtpTimestamp(), renderTimeMs });

    auto buffer = webrtc::scoped_refptr<media::DecoderBuffer>(
        new webrtc::RefCountedObject<media::DecoderBuffer>(
            inputImage.data(), inputImage.size()));
    av1Decoder->SetStream(streamId++, buffer);

    auto result = av1Decoder->Decode();
    if (result == media::AcceleratedVideoDecoder::kConfigChange) {

        const gfx::Size picSize = av1Decoder->GetPicSize();
        if (!recreateDecoder(picSize.width(), picSize.height(),
                             av1Decoder->GetProfile())) {
            LOG_ERROR("[D3D11Av1VideoDecoder] recreateDecoder failed");
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        result = av1Decoder->Decode();
    }
    if (result == media::AcceleratedVideoDecoder::kDecodeError) {
        LOG_ERROR("[D3D11Av1VideoDecoder] AV1 decode error (streamId=%d inBytes=%zu rtp=%u "
                  "pic=%dx%d profile=%d chroma=%d bitdepth=%d)",
                  streamId - 1, inputImage.size(), inputImage.RtpTimestamp(),
                  av1Decoder->GetPicSize().width(),
                  av1Decoder->GetPicSize().height(),
                  static_cast<int>(av1Decoder->GetProfile()),
                  static_cast<int>(av1Decoder->GetChromaSampling()),
                  static_cast<int>(av1Decoder->GetBitDepth()));

        av1Decoder->Reset();
        if (!metaQueue.empty()) metaQueue.pop_front();   // 丢弃该帧时间戳,避免后续错位
        // 零拷贝解码失败(GPU 崩/设备移除/驱动不支持共享纹理解码) -> 切拷贝路径。
        if (decodePath == DecodePath::ZeroCopy) {
            LOG_WARN("[D3D11Av1VideoDecoder] zero-copy decode error -> switch to copy path");
            switchToCopyMode();
        }
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (result == media::AcceleratedVideoDecoder::kRanOutOfSurfaces) {

        LOG_ERROR("[D3D11Av1VideoDecoder] kRanOutOfSurfaces: slot pool exhausted (drop frame, NO_OUTPUT)");

        if (!metaQueue.empty()) metaQueue.pop_front();

        return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

bool D3D11Av1VideoDecoder::outputFrame(Slot* slot, const media::AV1Picture& picture) {
    if (!slot || !decodeCallback || !renderDevice) {
        LOG_ERROR("[D3D11Av1VideoDecoder] outputFrame guard failed: slot=%d callback=%d renderDevice=%d",
                  slot != nullptr, decodeCallback != nullptr, renderDevice != nullptr);
        return false;
    }

    uint32_t rtp = 0;
    int64_t renderMs = 0;
    if (!metaQueue.empty()) {
        rtp = metaQueue.front().first;
        renderMs = metaQueue.front().second;
        metaQueue.pop_front();
    }

    std::shared_ptr<D3D11VideoFrameData> data = std::make_shared<D3D11VideoFrameData>();
    data->width = (int)picture.frameHeader.width;
    data->height = (int)picture.frameHeader.height;

    // 渲染读的共享纹理:ZeroCopy 是解码直写 texture;Copy 是 VideoProcessor 拷贝后的 sharedTexture。
    // 渲染端对象每槽惰性缓存,不再每帧创建。
    ID3D11Texture2D* sharedSource =
        (decodePath == DecodePath::Copy) ? slot->sharedTexture.Get() : slot->texture.Get();
    if (slot->renderDeviceCached != renderDevice.Get()) {
        slot->renderTexture.Reset();
        slot->planeYSrv.Reset();
        slot->planeUvSrv.Reset();
        slot->renderDeviceCached = nullptr;
        if (!openSharedTexForRender(sharedSource, slot->renderTexture,
                                    slot->planeYSrv, slot->planeUvSrv)) {
            LOG_ERROR("[D3D11Av1VideoDecoder] openSharedTexForRender failed");
            return false;
        }
        slot->renderDeviceCached = renderDevice.Get();
    }
    // 等解码完成(查询在 submitDecode 的 DecoderEndFrame 后 End),保证渲染端读完整帧。
    // 自旋用 yield(GPU 快时立即返回,不加延迟);copyQuery 缺失时绝不能交付未完成帧 -> 拒交。
    if (copyQuery) {
        while (decodeContext->GetData(copyQuery.Get(), nullptr, 0, 0) == S_FALSE) {
            std::this_thread::yield();
        }
    } else {
        LOG_ERROR("[D3D11Av1VideoDecoder] copyQuery missing, cannot sync decode -> refuse to deliver");
        return false;
    }
    data->nv12Texture = slot->renderTexture;
    data->planeYSrv = slot->planeYSrv;
    data->planeUvSrv = slot->planeUvSrv;

    // 帧持有 picture 引用:槽位在渲染完成前不释放(RAII 回池)。
    webrtc::scoped_refptr<media::AV1Picture> keepReference(
        const_cast<media::AV1Picture*>(&picture));
    data->keepAlive =
        std::shared_ptr<webrtc::scoped_refptr<media::AV1Picture>>(
            new webrtc::scoped_refptr<media::AV1Picture>(std::move(keepReference)));

    // 硬解帧直投 widget(绕过 track-sink);WebRTC 簿记由下方 Decoded() 照常维护。
    if (onDisplayHandle) {
        std::shared_ptr<VideoFrame> appFrame = std::make_shared<VideoFrame>();
        appFrame->format = FrameFormat::Nv12Gpu;
        appFrame->d3d11FrameData = data;
        appFrame->width = data->width;
        appFrame->height = data->height;
        onDisplayHandle(appFrame);
    }

    // WebRTC 簿记照旧(VCM frame_infos_ 弹出等依赖 Decoded())。
    webrtc::scoped_refptr<WebrtcD3D11TextureBuffer> d3d11TextureBuffer =
        webrtc::make_ref_counted<WebrtcD3D11TextureBuffer>(std::move(data));
    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(d3d11TextureBuffer)
                                   .set_timestamp_rtp(rtp)
                                   .set_timestamp_ms(renderMs)
                                   .build();
    decodeCallback->Decoded(frame);
    return true;
}

void D3D11Av1VideoDecoder::setD3D11Device(ID3D11Device* dev) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!dev) return;

    // 同一个设备(同连接内重复转发)只唤醒,避免无谓重建。
    if (renderDevice.Get() == dev) {
        renderCv.notify_all();
        return;
    }
    renderDevice = dev;
    renderCv.notify_all();  // 唤醒 Decode() 中等待渲染设备的首帧

    // 设备指针变化 = 每次新连接都是新窗口/新 QRhi 设备。
    // 必须重建解码器,否则沿用上一连接的旧设备(可能已销毁/移除) -> 解码/共享异常。
    // 旧的 "仅适配器不同才重建" 在两次连接用同一张卡时会跳过重建,正是这个 bug。
    if (initialized) {
        LOG_WARN("[D3D11Av1VideoDecoder] render device changed (new connection), rebuilding");
        av1Decoder.reset();   // 先释放 picture(回槽位),避免销毁槽位后 returnSlot 悬垂
        videoDecoder.Reset();
        videoProcessor.Reset();
        vpEnumerator.Reset();
        vpWidth = vpHeight = 0;
        destroySlots();
        decodeContext.Reset();
        videoDevice.Reset();
        videoContext.Reset();
        decodeDevice.Reset();
        initialized = false;
        ensureInitialized();
    } else {
        ensureInitialized();
    }
}

int32_t D3D11Av1VideoDecoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
    decodeCallback = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t D3D11Av1VideoDecoder::Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released.store(true, std::memory_order_release);
    renderCv.notify_all();   // 唤醒 Decode() 中等待渲染设备的线程
    destroySlots();
    videoDecoder.Reset();
    videoProcessor.Reset();
    vpEnumerator.Reset();
    vpWidth = vpHeight = 0;
    videoContext.Reset();
    videoDevice.Reset();
    decodeContext.Reset();
    decodeDevice.Reset();
    renderDevice.Reset();
    av1Decoder.reset();
    metaQueue.clear();
    initialized = false;
    decodePath = DecodePath::ZeroCopy;   // 复用解码器时重置为默认零拷贝
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        slots.clear();   // 释放全部 unique_ptr 槽位对象(池彻底销毁)
        poolEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo D3D11Av1VideoDecoder::GetDecoderInfo() const {
    DecoderInfo info;
    info.implementation_name = "D3D11(AV1/DXVA)";
    info.is_hardware_accelerated = true;
    return info;
}

} // namespace rtc
} // namespace hope
