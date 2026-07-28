#pragma once
#include <boost/asio/detail/socket_ops.hpp>
#include "Utils.h"

namespace hope {

    namespace rtc {

        enum class WebrtcRequestState {
            REGISTER = 0,
            REQUEST = 1,
            RESTART = 2,
            STOPR_EMOTE = 3,
            START = 4,
            CLOSE = 5,
            CLOSE_SYSTEM = 6,
            SYSTEM_READLY = 7,
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

        class WriterData {

        public:

            WriterData(char* data, size_t size) : size(size) {

                this->data = new char[size + sizeof(int64_t)];

                uint64_t size64t = boost::asio::detail::socket_ops::host_to_network_long(
                    static_cast<uint64_t>(size));

                fastCopy(this->data, &size64t, sizeof(uint64_t));

                fastCopy(this->data + sizeof(uint64_t), data, size);

                this->size = size + sizeof(int64_t);

            };

            ~WriterData() {

                if (data != nullptr) {

                    delete[] data;

                    data = nullptr;

                }
            }

            char* data;

            size_t size;
        };

    }

}