#include "CreateDescriptionObserverImpl.h"

#include "WebrtcManager.h"

#include "Utils.h"

namespace hope {

	namespace rtc {

        // CreateOfferObserverImpl实现
        void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
            if (!desc) {
                LOG_ERROR("CreateOffer success callback received null description");
                return;
            }

            // 获取并修改 SDP(纯字符串操作,当前线程安全)
            std::string sdp;
            desc->ToString(&sdp);

            // 为 playout-delay 扩展添加延迟参数（min=0ms, max=0ms）
            size_t playoutDelayPos = sdp.find("http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");
            if (playoutDelayPos != std::string::npos) {
                size_t lineStart = sdp.rfind("\r\na=extmap:", playoutDelayPos);
                size_t lineEnd = sdp.find("\r\n", playoutDelayPos);
                if (lineStart != std::string::npos && lineEnd != std::string::npos) {
                    std::string extmapLine = sdp.substr(lineStart, lineEnd - lineStart);
                    std::string modifiedLine = extmapLine + ";min=0;max=0";
                    sdp.replace(lineStart, lineEnd - lineStart, modifiedLine);
                    LOG_INFO("Added playout delay optimization: min=0;max=0");
                }
            }

            // 用修改后的 SDP 重新创建 SessionDescription
            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::SessionDescriptionInterface> modifiedDesc =
                webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

            // 快照 ref,避免看门狗 releaseSource 替换成员时悬垂;直调(不 post),信令即时发出。
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnectionRef = peerConnection;
            if (peerConnectionRef && modifiedDesc) {
                LOG_INFO("Set modified SDP with playout delay optimization");
                peerConnectionRef->SetLocalDescription(SetLocalDescriptionObserver::Create().get(),
                    modifiedDesc.release());
            } else {
                LOG_ERROR("Failed to parse modified SDP: %s", error.description.c_str());
            }

            // 发送信令(与 System 直调版本一致,避免 post 延迟窗口丢 answer/offer)
            boost::json::object msg;
            msg["type"] = "offer";
            msg["sdp"] = sdp;
            webrtcManager->sendSignalingMessage(msg);
        }

        void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
            LOG_ERROR("CreateOffer failed: %s", error.message());
        }

        // CreateAnswerObserverImpl实现
        void CreateAnswerObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
            if (!desc) {
                LOG_ERROR("CreateAnswer success callback received null description");
                return;
            }

            std::string sdp;
            if (!desc->ToString(&sdp)) {
                LOG_ERROR("Failed to convert answer to string");
                return;
            }

            // 快照 ref,避免看门狗 releaseSource 替换成员时悬垂;直调 SetLocalDescription,
            // desc 所有权交给 libwebrtc。与 System 直调版本一致,不 post,避免延迟窗口丢 answer。
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnectionRef = peerConnection;
            peerConnectionRef->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

            boost::json::object msg;
            msg["type"] = "answer";
            msg["sdp"] = sdp;
            webrtcManager->sendSignalingMessage(msg);
        }

        void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
            LOG_ERROR("CreateAnswer failed: %s", error.message());
        }

	}

}
