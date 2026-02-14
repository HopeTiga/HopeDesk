#include "RTCStatsCollectorHandle.h"

#include "Utils.h"

namespace hope {

	namespace rtc {

        void RTCStatsCollectorHandle::OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
            std::string selectedPairId;

            // 1. 遍历报告，寻找 Transport Stats 以获取选中的 Candidate Pair ID
            for (const auto& stat : *report) {
                if (stat.type() == webrtc::RTCTransportStats::kType) {
                    const auto& transport = stat.cast_to<webrtc::RTCTransportStats>();

                    // 使用 .has_value() 检查是否存在
                    if (transport.selected_candidate_pair_id.has_value()) {
                        selectedPairId = *transport.selected_candidate_pair_id;
                        break;
                    }
                }
            }

            if (selectedPairId.empty()) {
                LOG_WARNING("No selected candidate pair yet (Connection might not be ready).");
                return;
            }

            // 2. 根据 ID 找到选中的 Candidate Pair
            const webrtc::RTCStats* pairStat = report->Get(selectedPairId);
            if (!pairStat) return;

            const auto& candidatePair = pairStat->cast_to<webrtc::RTCIceCandidatePairStats>();

            // 获取本地和远端候选者的 ID
            std::string localCandidateId = *candidatePair.local_candidate_id;
            std::string remoteCandidateId = *candidatePair.remote_candidate_id;

            // 3. 查找具体的 Candidate 对象并判断类型
            const webrtc::RTCStats* localCandStat = report->Get(localCandidateId);
            const webrtc::RTCStats* remoteCandStat = report->Get(remoteCandidateId);

            if (localCandStat && remoteCandStat) {
                const auto& localCand = localCandStat->cast_to<webrtc::RTCIceCandidateStats>();
                const auto& remoteCand = remoteCandStat->cast_to<webrtc::RTCIceCandidateStats>();

                std::string localType = *localCand.candidate_type;
                std::string remoteType = *remoteCand.candidate_type;

                LOG_INFO("Active Connection Info:");

                // 注意：printf 风格需要使用 %s，并且 string 必须调用 .c_str()
                LOG_INFO("  Local Type: %s | IP: %s",
                     localType.c_str(),
                     localCand.ip->c_str());

                LOG_INFO("  Remote Type: %s | IP: %s",
                     remoteType.c_str(),
                     remoteCand.ip->c_str());

                // 4. 判断逻辑
                if (localType == "relay" || remoteType == "relay") {
                    LOG_INFO("==> Connection Type: TURN (Relayed)");

                    if(onRTCStatsCollectorHandle){

                        onRTCStatsCollectorHandle(1);

                    }

                }
                else {
                    LOG_INFO("==> Connection Type: P2P (STUN/Direct)");

                    if(onRTCStatsCollectorHandle){

                        onRTCStatsCollectorHandle(0);

                    }
                }
            }
        }
	}

}
