#include "PeerConnectionObserverImpl.h"

#include <boost/json.hpp>

#include "WebrtcManager.h"
#include "RTCStatsCollectorHandle.h"
#include "Utils.h"

namespace hope {
namespace rtc {

PeerConnectionObserverImpl::PeerConnectionObserverImpl(WebrtcManager* webrtcManager)
    : webrtcManager(webrtcManager) {}

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
    LOG_INFO("Data channel received: %s", dataChannel->label().c_str());

    if (dataChannel->label() == "dataChannel") {
        webrtcManager->dataChannel = dataChannel;
        // 新连接:清空 cursor 缓存,使 index 与对端(每连接从 0)重新对齐
        webrtcManager->resetCursorCache();
        webrtcManager->dataChannelObserver = std::make_unique<DataChannelObserverImpl>();
        webrtcManager->dataChannelObserver->setOnDataHandle(
            std::bind(&WebrtcManager::handleCursor, webrtcManager, std::placeholders::_1, std::placeholders::_2));
        dataChannel->RegisterObserver(webrtcManager->dataChannelObserver.get());
    }
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

    webrtcManager->sendSignalingMessage(msg);
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) {
    switch (newState) {
    case webrtc::PeerConnectionInterface::kIceConnectionConnected: {
        LOG_INFO("WebRTC connection established");
        webrtcManager->isRemote = true;
        webrtcManager->rtcStatsCollectorHandle = webrtc::make_ref_counted<hope::rtc::RTCStatsCollectorHandle>();
        if (webrtcManager->onRTCStatsCollectorHandle) {
            webrtcManager->rtcStatsCollectorHandle->onRTCStatsCollectorHandle = webrtcManager->onRTCStatsCollectorHandle;
        }
        webrtcManager->peerConnection->GetStats(webrtcManager->rtcStatsCollectorHandle.get());
        if (webrtcManager->onRemoteSuccessFulHandle) {
            webrtcManager->onRemoteSuccessFulHandle();
        }
        break;
    }
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
        LOG_ERROR("ICE connection failed");
        // ICE 失败是终态:走 disConnectRemoteHandler 强制重置连接态(关 tcpSocket/peerConnection、
        // 重建空白 peerConnection),若之前已连上还会通知 UI。防止残留导致重连失败。
        webrtcManager->disConnectRemoteHandler();
        break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: {
        LOG_WARN("ICE connection disconnected");
        webrtcManager->disConnectRemoteHandler();
        break;
    }
    case webrtc::PeerConnectionInterface::kIceConnectionClosed: {
        LOG_INFO("ICE connection closed");
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
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
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
        webrtcManager->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
            static_cast<webrtc::VideoTrackInterface*>(track.release()));
        webrtcManager->videoSinkImpl = std::make_unique<VideoTrackSinkImpl>(webrtcManager);
        webrtcManager->videoTrack->AddOrUpdateSink(webrtcManager->videoSinkImpl.get(), webrtc::VideoSinkWants());
        return;
    }

    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        LOG_INFO("Audio track received");
        receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
        webrtcManager->audioTrack = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(
            static_cast<webrtc::AudioTrackInterface*>(track.release()));
    }
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

void PeerConnectionObserverImpl::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {

}

void PeerConnectionObserverImpl::OnIceCandidateError(const std::string& address, int port,
                                                     const std::string& url, int errorCode,
                                                     const std::string& errorText) {
    LOG_ERROR("PeerConnectionObserverImpl::OnIceCandidateError: address=%s, port=%d, url=%s, errorCode=%d, errorText=%s",
              address.c_str(), port, url.c_str(), errorCode, errorText.c_str());
}

} // namespace rtc
} // namespace hope