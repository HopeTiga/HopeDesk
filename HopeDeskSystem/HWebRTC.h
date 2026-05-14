#pragma once
#include <boost/asio/detail/socket_ops.hpp>
#include "Utils.h"

namespace hope {

	namespace rtc {

        enum class WebRTCRemoteState {
            nullRemote = 0,
            masterRemote = 1,
            followerRemote = 2,
        };

        enum class WebRTCConnetState {
            none,
            connect,
        };

        enum class WebRTCRequestState {
            REGISTER = 0,
            REQUEST = 1,
            RESTART = 2,
            STOPREMOTE = 3,
            START = 4,
            CLOSE = 5,
            CLOSESYSTEM = 6,
            SYSTEMREADLY = 7,
            STATS = 8
        };

        enum class WebRTCVideoCodec {
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