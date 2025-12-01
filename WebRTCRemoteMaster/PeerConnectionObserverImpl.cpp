#include "PeerConnectionObserverImpl.h"

#include <boost/json.hpp>

#include "WebRTCManager.h"

#include "Logger.h"

namespace hope {

	namespace rtc {

    void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::kStable:
            Logger::getInstance()->info("Signaling state: kStable");
            break;
        case webrtc::PeerConnectionInterface::kHaveLocalOffer:
            break;
        case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
            break;
        case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
            break;
        case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
            break;
        case webrtc::PeerConnectionInterface::kClosed:
            Logger::getInstance()->info("Signaling state: kClosed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) {
        Logger::getInstance()->info("Data channel received: " + dataChannel->label());

        manager->dataChannel = dataChannel;
        manager->dataChannelObserver = std::make_unique<DataChannelObserverImpl>(manager);
        dataChannel->RegisterObserver(manager->dataChannelObserver.get());
    }

    void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::kIceGatheringComplete:
            Logger::getInstance()->info("ICE gathering complete");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
        if (!candidate) {
            Logger::getInstance()->error("OnIceCandidate called with null candidate");
            return;
        }

        std::string sdp;
        if (!candidate->ToString(&sdp)) {
            Logger::getInstance()->error("Failed to convert ICE candidate to string");
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
        case webrtc::PeerConnectionInterface::kIceConnectionConnected:
            Logger::getInstance()->info("WebRTC connection established");

            break;
        case webrtc::PeerConnectionInterface::kIceConnectionFailed:
            Logger::getInstance()->error("ICE connection failed");
            break;
        case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
            Logger::getInstance()->warning("ICE connection disconnected");
            break;
        case webrtc::PeerConnectionInterface::kIceConnectionClosed:
            Logger::getInstance()->info("ICE connection closed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:{

            Logger::getInstance()->info("Peer connection established");

            manager->isRemote = true;

            manager->state = WebRTCRemoteState::masterRemote;

            if(manager->remoteSuccessFulHandle){

                manager->remoteSuccessFulHandle();

            }

            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:{

            manager->disConnectHandle();

            Logger::getInstance()->warning("Peer connection disconnected");

            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:

            manager->disConnectHandle();

            Logger::getInstance()->error("Peer connection failed");

            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:

            Logger::getInstance()->info("Peer connection closed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        auto receiver = transceiver->receiver();

        auto track = receiver->track();

        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            Logger::getInstance()->info("Video track received");
            receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
            manager->isReceive = true;
            manager->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
                static_cast<webrtc::VideoTrackInterface*>(track.release())
                );
            manager->videoSinkImpl = std::make_unique<VideoTrackSinkImpl>(manager);
            manager->videoTrack->AddOrUpdateSink(manager->videoSinkImpl.get(), webrtc::VideoSinkWants());
            manager->isReceive = true;
        }
    }

    void PeerConnectionObserverImpl::OnIceCandidateRemoved(const webrtc::IceCandidate* candidate) {
        // 空实现
    }

    void PeerConnectionObserverImpl::OnIceCandidatesRemoved(const std::vector<webrtc::Candidate>& candidates) {
        // 空实现
    }

    void PeerConnectionObserverImpl::OnIceConnectionReceivingChange(bool receiving) {
        // 空实现
    }

    void PeerConnectionObserverImpl::OnIceSelectedCandidatePairChanged(const webrtc::CandidatePairChangeEvent& event) {
        // 空实现
    }

    void PeerConnectionObserverImpl::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                                                const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
        // 空实现
    }

    void PeerConnectionObserverImpl::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
        // 空实现
    }

	}

}
