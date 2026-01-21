#include "PeerConnectionObserverImpl.h"

#include <boost/json.hpp>

#include "WebRTCManager.h"

#include "Utils.h"

namespace hope {

	namespace rtc {

    void PeerConnectionObserverImpl::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::kStable:
            LOG_INFO("Signaling state: kStable");
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
            LOG_INFO("Signaling state: kClosed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel) {

        LOG_INFO("Data channel received: %s" ,dataChannel->label().c_str());

        if(dataChannel->label() == "dataChannel"){

            manager->dataChannel = dataChannel;

            manager->dataChannelObserver = std::make_unique<DataChannelObserverImpl>(manager);

            manager->dataChannelObserver->setOnDataHandle(std::bind(&WebRTCManager::handleCursor,manager,std::placeholders::_1,std::placeholders::_2));

            dataChannel->RegisterObserver(manager->dataChannelObserver.get());

            return;
        }

        return;

    }

    void PeerConnectionObserverImpl::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::kIceGatheringComplete:
            LOG_INFO("ICE gathering complete");
            break;
        default:
            break;
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
        case webrtc::PeerConnectionInterface::kIceConnectionConnected:
            LOG_INFO("WebRTC connection established");

            break;
        case webrtc::PeerConnectionInterface::kIceConnectionFailed:
            LOG_ERROR("ICE connection failed");
            break;
        case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
            LOG_WARNING("ICE connection disconnected");
            break;
        case webrtc::PeerConnectionInterface::kIceConnectionClosed:
            LOG_INFO("ICE connection closed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState newState) {
        switch (newState) {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:{

            LOG_INFO("Peer connection established");

            manager->isRemote = true;

            manager->state = WebRTCRemoteState::masterRemote;

            if(manager->remoteSuccessFulHandle){

                manager->remoteSuccessFulHandle();

            }

            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:{

            manager->disConnectHandle();

            LOG_WARNING("Peer connection disconnected");

            break;
        }
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:

            manager->disConnectHandle();

            LOG_ERROR("Peer connection failed");

            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:

            LOG_INFO("Peer connection closed");
            break;
        default:
            break;
        }
    }

    void PeerConnectionObserverImpl::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        auto receiver = transceiver->receiver();

        auto track = receiver->track();

        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            LOG_INFO("Video track received");
            receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
            manager->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
                static_cast<webrtc::VideoTrackInterface*>(track.release())
                );
            manager->videoSinkImpl = std::make_unique<VideoTrackSinkImpl>(manager);
            manager->videoTrack->AddOrUpdateSink(manager->videoSinkImpl.get(), webrtc::VideoSinkWants());
            return;
        }

        if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
            LOG_INFO("Video track received");
            receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
            manager->audioTrack = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(
                static_cast<webrtc::AudioTrackInterface*>(track.release())
                );
            return;
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

    void PeerConnectionObserverImpl::OnIceCandidateError(const std::string& address, int port, const std::string& url,
                                                         int errorCode, const std::string& errorText) {

        LOG_ERROR("PeerConnectionObserverImpl::OnIceCandidateError: address=%s, port=%d, url=%s, errorCode=%d, errorText=%s\n",
               address.c_str(), port, url.c_str(), errorCode, errorText.c_str());

    }

	}

}
