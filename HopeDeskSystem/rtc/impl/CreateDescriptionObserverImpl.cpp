#include "CreateDescriptionObserverImpl.h"

#include "../WebrtcManager.h"

#include "../../utils/Utils.h"

namespace hope {

    namespace rtc {

        // CreateOfferObserverImpl实现
        void CreateOfferObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
            if (!desc) {
                LOG_ERROR("CreateOffer Success Callback Received Null Description");
                return;
            }

            // 获取并修改 SDP
            std::string sdp;
            desc->ToString(&sdp);

            // 为 playout-delay 扩展添加延迟参数（min=0ms, max=0ms）
            // 在 video m-line 中查找 playout-delay 扩展
            size_t playoutDelayPos = sdp.find("http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");
            if (playoutDelayPos != std::string::npos) {
                // 找到对应的 extmap 行
                size_t lineStart = sdp.rfind("\r\na=extmap:", playoutDelayPos);
                size_t lineEnd = sdp.find("\r\n", playoutDelayPos);

                if (lineStart != std::string::npos && lineEnd != std::string::npos) {
                    std::string extmapLine = sdp.substr(lineStart, lineEnd - lineStart);
                    // 添加延迟参数: min=0;max=0 表示最低延迟
                    std::string modifiedLine = extmapLine + ";min=0;max=0";
                    sdp.replace(lineStart, lineEnd - lineStart, modifiedLine);

                    LOG_INFO("Added Playout Delay Optimization: Min=0;Max=0");
                }
            }

            // 用修改后的 SDP 重新创建 SessionDescription
            webrtc::SdpParseError error;
            std::unique_ptr<webrtc::SessionDescriptionInterface> modifiedDesc =
                webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);

            if (modifiedDesc) {
                LOG_INFO("Set Modified SDP With Playout Delay Optimization");
                peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(),
                    modifiedDesc.release());
            }
            else {
                LOG_ERROR("Failed To Parse Modified SDP: {}", error.description.c_str());
                // 如果修改失败，使用原始描述
                peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);
            }

            // 发送信令
            boost::json::object msg;
            msg["type"] = "offer";
            msg["sdp"] = sdp;
            webrtcManager->sendSignalingMessage(msg);
        }

        void CreateOfferObserverImpl::OnFailure(webrtc::RTCError error) {
            LOG_ERROR("CreateOffer Failed: {}", error.message());
        }

        // CreateAnswerObserverImpl实现
        void CreateAnswerObserverImpl::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
            if (!desc) {
                LOG_ERROR("CreateAnswer Success Callback Received Null Description");
                return;
            }

            peerConnection->SetLocalDescription(SetLocalDescriptionObserver::Create().get(), desc);

            std::string sdp;
            if (!desc->ToString(&sdp)) {
                LOG_ERROR("Failed To Convert Answer To String");
                return;
            }

            boost::json::object msg;
            msg["type"] = "answer";
            msg["sdp"] = sdp;

            webrtcManager->sendSignalingMessage(msg);
        }

        void CreateAnswerObserverImpl::OnFailure(webrtc::RTCError error) {
            LOG_ERROR("CreateAnswer Failed: {}", error.message());
        }

    }

}