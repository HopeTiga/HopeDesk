#pragma once
#include <boost/asio/detail/socket_ops.hpp>
#include "../utils/Utils.h"

namespace hope {

    namespace rtc {

        enum class RequestType {
            SYSTEM_INIT = 0,
            REQUEST = 1,
            START = 4,
            STATS = 8,
            ENCODE_STATUS = 9   // System -> 被控 Native:上报当前编码 codec + 硬编/软编
        };

        enum class WebrtcVideoCodec {
            VP8,
            VP9,
            H264,
            H265,
            AV1,
        };

    }

}