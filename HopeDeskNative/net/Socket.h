#pragma once

#include <cstddef>
#include <cstring>
#include <string>

#include <boost/asio/detail/socket_ops.hpp>

namespace hope {

namespace net {

// 客户端 → 服务端:信令请求
struct WebrtcRequest {
    int requestType = 0;
    std::string accountId;
    std::string targetId;
    std::string payload;
};

// 服务端 → 客户端:信令响应
struct WebrtcResponse {
    int requestType = 0;
    int state = 0;
    std::string message;
    std::string accountId;
    std::string targetId;
    std::string payload;
};

class WriterData {
public:
    WriterData(const char* data, size_t size) : size(size) {
        this->data = new char[size + sizeof(int64_t)];

        uint64_t size64t = boost::asio::detail::socket_ops::network_to_host_long(
            static_cast<uint64_t>(size));

        std::memcpy(this->data, &size64t, sizeof(uint64_t));
        std::memcpy(this->data + sizeof(uint64_t), data, size);

        this->size = size + sizeof(int64_t);
    }

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
