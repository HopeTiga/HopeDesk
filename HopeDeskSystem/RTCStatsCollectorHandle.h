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

            std::function<void(int)> onRTCStatsCollectorHandle;
	
		};
	} // namespace rtc
} // namespace hope

