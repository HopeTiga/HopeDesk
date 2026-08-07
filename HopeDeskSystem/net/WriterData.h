#pragma once

#include <cstddef>
#include <cstring>

#include <boost/asio/detail/socket_ops.hpp>

namespace hope {

namespace net {

// 本地 TCP 通道的帧:int64 网络序长度前缀 + body。
// 构造时自动把 body 大小写入帧头,async_write 整帧写出,对端按同样协议拆帧。
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
