#include "PeerConnectionObserverImpl.h"

#include <boost/json.hpp>

#include "../WebrtcManager.h"
#include "RTCStatsCollectorHandle.h"
#include "../../utils/Utils.h"

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
    const std::string label = dataChannel->label();
    LOG_INFO("Data channel received: %s", label.c_str());

    if (label == "dataChannel") {
        std::shared_ptr<WebrtcManager> manager = webrtcManager->shared_from_this();
        // post 到 ioContext:回调在 WebRTC 信令线程,碰 WebrtcManager 状态须串行到 ioContext
        webrtcManager->post([manager, dataChannel = std::move(dataChannel)]() {
            // 新连接:清空 cursor 缓存,使 index 与对端(每连接从 0)重新对齐
            manager->resetCursorCache();
            manager->dataChannelObserver = std::make_unique<DataChannelObserverImpl>();
            manager->dataChannelObserver->setOnDataHandle(
                std::bind(&WebrtcManager::handleCursor, manager.get(), std::placeholders::_1, std::placeholders::_2));
            dataChannel->RegisterObserver(manager->dataChannelObserver.get());
            manager->dataChannel = std::move(dataChannel);
        });
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

    boost::json::object message;
    message["type"] = "candidate";
    message["candidate"] = sdp;
    message["mid"] = candidate->sdp_mid();
    message["mlineIndex"] = candidate->sdp_mline_index();

    std::shared_ptr<WebrtcManager> manager = webrtcManager->shared_from_this();
    webrtcManager->post([manager, message = std::move(message)]() mutable { manager->sendSignalingMessage(message); });
}

void PeerConnectionObserverImpl::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState newState) {
    std::shared_ptr<WebrtcManager> manager = webrtcManager->shared_from_this();
    // post 到 ioContext:回调在 WebRTC 信令线程,碰 WebrtcManager 状态须串行到 ioContext
    webrtcManager->post([manager, newState]() {
        switch (newState) {
        case webrtc::PeerConnectionInterface::kIceConnectionConnected: {
            LOG_INFO("WebRTC connection established");
            manager->isRemote = true;
            manager->rtcStatsCollectorHandle = webrtc::make_ref_counted<hope::rtc::RTCStatsCollectorHandle>();
            if (manager->onRTCStatsCollectorHandle) {
                manager->rtcStatsCollectorHandle->onRTCStatsCollectorHandle = manager->onRTCStatsCollectorHandle;
            }
            if (manager->peerConnection) {
                manager->peerConnection->GetStats(manager->rtcStatsCollectorHandle.get());
            }
            if (manager->onRemoteSuccessFulHandle) {
                manager->onRemoteSuccessFulHandle();
            }
            break;
        }
        case webrtc::PeerConnectionInterface::kIceConnectionFailed:
            LOG_ERROR("ICE connection failed");
            // ICE 失败是终态:走 disConnectRemoteHandler 强制重置连接态(关 tcpSocket/peerConnection、
            // 重建空白 peerConnection),若之前已连上还会通知 UI。防止残留导致重连失败。
            manager->disConnectRemoteHandler();
            break;
        case webrtc::PeerConnectionInterface::kIceConnectionDisconnected: {
            LOG_WARN("ICE connection disconnected");
            manager->disConnectRemoteHandler();
            break;
        }
        case webrtc::PeerConnectionInterface::kIceConnectionClosed: {
            LOG_INFO("ICE connection closed");
            break;
        }
        default:
            break;
        }
    });
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
    std::shared_ptr<WebrtcManager> manager = webrtcManager->shared_from_this();
    // post 到 ioContext:回调在 WebRTC 信令线程,碰 WebrtcManager 状态须串行到 ioContext
    webrtcManager->post([manager, transceiver = std::move(transceiver)]() {
        webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver = transceiver->receiver();
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track = receiver->track();

        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            LOG_INFO("Video track received");
            receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
            manager->videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
                static_cast<webrtc::VideoTrackInterface*>(track.release()));
            manager->videoSinkImpl = std::make_unique<VideoTrackSinkImpl>(manager.get());
            manager->videoTrack->AddOrUpdateSink(manager->videoSinkImpl.get(), webrtc::VideoSinkWants());
            return;
        }

        if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
            LOG_INFO("Audio track received");
            receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.00));
            manager->audioTrack = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(
                static_cast<webrtc::AudioTrackInterface*>(track.release()));
        }
    });
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