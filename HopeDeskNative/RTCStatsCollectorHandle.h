#pragma once

#include "api/stats/rtc_stats.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h" // 包含具体的 Stats 对象定义
#include <functional>

namespace hope {
	namespace rtc {
		class RTCStatsCollectorHandle : public webrtc::RTCStatsCollectorCallback {

        private:

            void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

        public:

            // type: 0=P2P 1=Relay; rttMs: 网络往返延迟(毫秒), <0 表示本轮无数据
            std::function<void(int, double)> onRTCStatsCollectorHandle;
	
		};
	} // namespace rtc
} // namespace hope

