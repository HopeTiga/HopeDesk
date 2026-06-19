#include "PeerConnectionObserverImpl.h"

#include <boost/json.hpp>
#include <sstream>

#include "WebRTCManager.h"
#include "Utils.h"

namespace hope {
    namespace rtc {

        PeerConnectionObserverImpl::PeerConnectionObserverImpl(WebRTCManager* manager)
            : manager(manager) {
        }
  
        void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) {
            switch (newState) {
            case webrtc::PeerConnectionInterface::kClosed: {
                LOG_INFO("Signaling state: kClosed");
                break;
            }
            default:
                break;
            }
        }

        void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) {
     
        }

        void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) {
            if (newState == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
                LOG_INFO("ICE gathering complete");
            }
        }

        void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
            if (!candidate) {
                LOG_ERROR("OnIceCandidate called with null candidate");
                return;
            }

            std::string sdp;
            if (!candidate->ToString(&sdp)) {
                LOG_ERROR("Failed to convert ICE candidate to string");
                return;
            }

            boost::json::object msg;
            msg["type"] = "candidate";
            msg["candidate"] = sdp;
            msg["mid"] = candidate->sdp_mid();
            msg["mlineIndex"] = candidate->sdp_mline_index();

            manager->sendSignalingMessage(msg);
        }

        void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) {
            switch (newState) {
            case webrtc::PeerConnectionInterface::kIceConnectionConnected: {
                LOG_INFO("ICE connection established");

                auto localDesc = manager->peerConnection->local_description();
                if (localDesc) {
                    std::string sdp;
                    localDesc->ToString(&sdp);

                    std::istringstream stream(sdp);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (line.find("a=rtpmap:") == 0 && line.find("/90000") != std::string::npos) {
                            size_t spacePos = line.find(' ');
                            if (spacePos != std::string::npos) {
                                std::string codecInfo = line.substr(spacePos + 1);
                                size_t slashPos = codecInfo.find('/');
                                if (slashPos != std::string::npos) {
                                    std::string codecName = codecInfo.substr(0, slashPos);
                                    LOG_INFO("=== Video codec actually being used: %s ===", codecName.c_str());
                                    break;
                                }
                            }
                        }
                    }
                }

                boost::json::object json;
                json["requestType"] = static_cast<int64_t>(WebRTCRequestState::START);
                std::string jsonStr = boost::json::serialize(json);
                std::shared_ptr<WriterData> data = std::make_shared<WriterData>(const_cast<char*>(jsonStr.c_str()), jsonStr.size());
                manager->asyncWrite(data);

                manager->rtcStatsCollectorHandle = webrtc::make_ref_counted<hope::rtc::RTCStatsCollectorHandle>();
                manager->rtcStatsCollectorHandle->onRTCStatsCollectorHandle = [this](int connectionType) {
                    boost::json::object json;
                    json["requestType"] = static_cast<int64_t>(WebRTCRequestState::STATS);
                    json["connectionType"] = connectionType;
                    std::string jsonStr = boost::json::serialize(json);
                    std::shared_ptr<WriterData> data = std::make_shared<WriterData>(const_cast<char*>(jsonStr.c_str()), jsonStr.size());
                    manager->asyncWrite(data);
                    };
                manager->peerConnection->GetStats(manager->rtcStatsCollectorHandle.get());
                break;
            }

            case webrtc::PeerConnectionInterface::kIceConnectionFailed:
                LOG_ERROR("ICE connection failed");
                break;
            case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: {
                LOG_INFO("ICE connection disconnected");
                boost::json::object json;
                json["requestType"] = static_cast<int64_t>(WebRTCRequestState::CLOSE);
                std::string jsonStr = boost::json::serialize(json);
                std::shared_ptr<WriterData> data = std::make_shared<WriterData>(const_cast<char*>(jsonStr.c_str()), jsonStr.size());
                manager->asyncWrite(data);
                break;
            }
            default:
                break;
            }
        }

        void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) {
            switch (newState) {
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
                LOG_INFO("Peer connection established");
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
                LOG_ERROR("Peer connection failed");
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
                break;
            default:
                break;
            }
        }

        void PeerConnectionObserverImpl::OnIceCandidateError(const std::string& address, int port, const std::string& url,
            int errorCode, const std::string& errorText) {
            printf("PeerConnectionObserverImpl::OnIceCandidateError: address=%s, port=%d, url=%s, errorCode=%d, errorText=%s\n",
                address.c_str(), port, url.c_str(), errorCode, errorText.c_str());
        }

        void PeerConnectionObserverImpl::OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {

        }

        void PeerConnectionObserverImpl::OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {

        }

        void PeerConnectionObserverImpl::OnRenegotiationNeeded() {

        }

        void PeerConnectionObserverImpl::OnNegotiationNeededEvent(uint32_t eventId) {

        }

        void PeerConnectionObserverImpl::OnStandardizedIceConnectionChange(
            webrtc::PeerConnectionInterface::IceConnectionState newState) {

        }

        void PeerConnectionObserverImpl::OnInterestingUsage(int usagePattern) {
  
        }

        void PeerConnectionObserverImpl::OnIceCandidateRemoved(const webrtc::IceCandidate* candidate) {
   
        }

        void PeerConnectionObserverImpl::OnIceConnectionReceivingChange(bool receiving) {
   
        }

        void PeerConnectionObserverImpl::OnIceSelectedCandidatePairChanged(
            const webrtc::CandidatePairChangeEvent& event) {

        }

        void PeerConnectionObserverImpl::OnAddTrack(
            webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
            const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {

        }

        void PeerConnectionObserverImpl::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {

        }

        void PeerConnectionObserverImpl::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {

        }

    } // namespace rtc
} // namespace hope