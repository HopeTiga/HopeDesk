#include "RTCStatsCollectorHandle.h"

#include "../../utils/Utils.h"

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
                LOG_WARN("No selected candidate pair yet (Connection might not be ready).");
                return;
            }

            // 2. 根据 ID 找到选中的 Candidate Pair
            const webrtc::RTCStats* pairStat = report->Get(selectedPairId);
            if (!pairStat) return;

            const auto& candidatePair = pairStat->cast_to<webrtc::RTCIceCandidatePairStats>();

            // 网络 RTT(秒):consent freshness / STUN 往返,转毫秒;无值时用 -1 表示未知
            double rttMs = -1.0;
            if (candidatePair.current_round_trip_time.has_value()) {
                rttMs = *candidatePair.current_round_trip_time * 1000.0;
            }

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

                // 4. 判断逻辑
                if (localType == "relay" || remoteType == "relay") {

                    if(onRTCStatsCollectorHandle){

                        onRTCStatsCollectorHandle(1, rttMs);

                    }

                }
                else {

                    if(onRTCStatsCollectorHandle){

                        onRTCStatsCollectorHandle(0, rttMs);

                    }
                }
            }
        }
	}

}
