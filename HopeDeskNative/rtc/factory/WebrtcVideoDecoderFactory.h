#pragma once
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "media/engine/internal_decoder_factory.h"

#include <wrl/client.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

struct ID3D11Device;

namespace hope {

	namespace rtc {

        class NvdecDecoder;
        class D3D11Av1VideoDecoder;
        struct VideoFrame;

        class WebrtcVideoDecoderFactory : public webrtc::VideoDecoderFactory {

		public:

            WebrtcVideoDecoderFactory();

			std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;

			CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
				bool reference_scaling) const;

			std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

			// 注入渲染端(QRhi)的 D3D11 设备,零拷贝解码->渲染用。
			// 转发给所有已创建且存活的解码器(可能先于 VideoWidget 初始化)。
			void setDecoderD3D11Device(ID3D11Device* dev);

			// 断开连接时清空缓存的渲染设备:避免下个连接复用上一连接(可能已销毁)的设备。
			void clearDecoderD3D11Device();

			// 释放前唤醒所有存活硬解解码器(置 released),必须在 peerConnection->Close() 前调用,
			// 否则解码线程卡死会让 Close() 永久阻塞(Stop 等待解码线程)。
			void wakeUpAllDecoders();

			// 解码器析构时从 liveAv1Decoders 注销(仅 D3D11Av1VideoDecoder 参与设备注入)。
			void removeDecoder(D3D11Av1VideoDecoder* d);

			// 硬解帧直投 widget(绕过 track-sink);转发给已创建的硬解解码器。
			void setOnDisplayHandle(std::function<void(std::shared_ptr<VideoFrame>)> onDisplayHandle);

			// NvdecDecoder 析构时从 liveNvdecDecoders 注销。
			void removeNvdecDecoder(NvdecDecoder* decoder);


		public:

			int webrtcEnableD3D11 = 0;  // 硬件解码(D3D11/MF),Native 本地

			// 解码状态 handle:(codec 名, 是否硬解)。Configure 后触发,供 UI 显示。
			std::function<void(const std::string& codec, bool hardDecode)> onDecoderStatusHandle;

		private:

			std::unique_ptr<webrtc::VideoDecoderFactory> internalDecoderFactory;
			// 强引用持有渲染端(QRhi)的 D3D11 设备:避免 QRhi 销毁后裸指针悬垂,
			// 二次连接把悬垂指针传给新解码器,在 ComPtr::AddRef 处崩溃。
			Microsoft::WRL::ComPtr<ID3D11Device> decoderD3D11Device;
			std::function<void(std::shared_ptr<VideoFrame>)> onDisplayHandle;   // 直投目标(工厂注入)
			std::vector<D3D11Av1VideoDecoder*> liveAv1Decoders;
			std::vector<NvdecDecoder*> liveNvdecDecoders;
			std::mutex decoderMutex;

		};

	}

}

