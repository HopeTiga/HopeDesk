#include "MsquicSocketClient.h"
#include "MsQuicApi.h"
#include "Utils.h"

namespace hope {
    namespace quic {

        MsquicSocketClient::MsquicSocketClient(boost::asio::io_context& ioContext)
            : ioContext(ioContext)
            , connection(nullptr)
            , stream(nullptr)
			, remoteStream(nullptr)
            , registration(nullptr)
            , configuration(nullptr)
            , serverPort(0)
            , payloadLen(0)
            , connected(false) {
        }

        MsquicSocketClient::~MsquicSocketClient() {
            clear();
        }

        bool MsquicSocketClient::initialize(const std::string& alpn) {

            if (MsQuic == nullptr) {

                return false;

            }

            this->alpn = alpn;

            return true;
        }

        bool MsquicSocketClient::connect(std::string serverAddress, uint64_t serverPort) {
            // 如果已有连接，先完全清理
            if (connection != nullptr) {
                LOG_INFO("reclear msquic connection");

                disconnect();

            }

            if (!registration) {

                registration = new MsQuicRegistration("MsquicSocketClient");

                if (!registration->IsValid()) {

                    return false;

                }

                // 创建注册
                registration = new MsQuicRegistration("MsquicSocketClient");
                if (!registration->IsValid()) {
                    return false;
                }

                // 配置设置
                MsQuicSettings settings;

                settings.SetIdleTimeoutMs(10000);

                settings.SetKeepAlive(5000);

                settings.SetPeerBidiStreamCount(2);

                // 创建ALPN
                MsQuicAlpn alpnBuffer(alpn.c_str());

                // 创建配置
                QUIC_CREDENTIAL_CONFIG credConfig = { 0 };
                credConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
                credConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

                configuration = new MsQuicConfiguration(
                    *registration,
                    alpnBuffer,
                    settings,
                    MsQuicCredentialConfig(credConfig)
                );

                if (!configuration->IsValid()) {

                    LOG_ERROR("MsQuicConfiguration Init Error");

                    return false;
                }

            }

            this->serverAddress = serverAddress;
            this->serverPort = serverPort;

            // 创建连接
            QUIC_STATUS status = MsQuic->ConnectionOpen(
                *registration,
                MsquicClientConnectionHandle,
                this,
                &connection);

            if (QUIC_FAILED(status)) {
                LOG_ERROR("ConnectionOpen failed: 0x%08X", status);
                return false;
            }

            // 启动连接
            status = MsQuic->ConnectionStart(
                connection,
                *configuration,
                QUIC_ADDRESS_FAMILY_INET,
                serverAddress.c_str(),
                serverPort);

            if (QUIC_FAILED(status)) {
                LOG_ERROR("ConnectionStart failed:0x%08X", status);
                MsQuic->ConnectionClose(connection);
                connection = nullptr;
                return false;
            }

            // 创建流
            stream = createStream();
            if (stream == nullptr) {
                LOG_ERROR("create MsquicStream failed");
                MsQuic->ConnectionClose(connection);
                connection = nullptr;
                return false;
            }

            connected.store(true);
            return true;
        }

        HQUIC MsquicSocketClient::createStream() {
            HQUIC stream = nullptr;
            QUIC_STATUS status = MsQuic->StreamOpen(
                connection,
                QUIC_STREAM_OPEN_FLAG_NONE,
                MsquicClientStreamHandle,
                this,
                &stream);

            if (QUIC_FAILED(status)) {
                return nullptr;
            }

            status = MsQuic->StreamStart(
                stream,
                QUIC_STREAM_START_FLAG_IMMEDIATE);

            if (QUIC_FAILED(status)) {
                MsQuic->StreamClose(stream);
                return nullptr;
            }

            return stream;
        }

        bool MsquicSocketClient::writeAsync(unsigned char* data, size_t size) {
            if (!connected || stream == nullptr) {
                return false;
            }

            QUIC_BUFFER* buffer = new QUIC_BUFFER;
            buffer->Buffer = data;
            buffer->Length = static_cast<uint32_t>(size);

            QUIC_STATUS status = MsQuic->StreamSend(
                stream,
                buffer,
                1,
                QUIC_SEND_FLAG_NONE,
                buffer);

            if (QUIC_FAILED(status)) {
                delete buffer;
                return false;
            }

            return true;
        }

        bool MsquicSocketClient::writeJsonAsync(const boost::json::object& json) {
            // 构建消息格式：length + body
            std::string body = boost::json::serialize(json);
            int64_t bodyLength = static_cast<int64_t>(body.size());
            size_t totalSize = sizeof(int64_t) + bodyLength;

            unsigned char* buffer = new unsigned char[totalSize];

            // 写入length
            *reinterpret_cast<int64_t*>(buffer) = bodyLength;

            // 写入body
            memcpy(buffer + sizeof(int64_t), body.data(), bodyLength);

            return writeAsync(buffer, totalSize);
        }


        void MsquicSocketClient::disconnect() {
            // 防止重复调用
            if (!connected.load() && connection == nullptr) {
                return;
            }

            // 立即标记逻辑断开，阻止新的写入
            connected.store(false);

            if (registration) {

                registration->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);

                registration = nullptr;

            }

            if (configuration) {

                delete configuration;

                configuration = nullptr;

            }

            if (stream) {
                // 使用中止标志立即关闭
                MsQuic->StreamShutdown(stream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                    QUIC_STATUS_SUCCESS);

                MsQuic->StreamClose(stream);

                stream = nullptr;
            }

            if (remoteStream) {

                MsQuic->StreamShutdown(remoteStream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                    QUIC_STATUS_SUCCESS);

                MsQuic->StreamClose(remoteStream);

                remoteStream = nullptr;
            }

            if (connection) {

                MsQuic->ConnectionShutdown(
                    connection,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
                    QUIC_STATUS_SUCCESS
                );

                MsQuic->ConnectionClose(connection);

                connection = nullptr;
            }

        }

        void MsquicSocketClient::setOnDataReceivedHandle(std::function<void(boost::json::object&)> handle) {
            onDataReceivedHandle = std::move(handle);
        }

        void MsquicSocketClient::setOnConnectionHandle(std::function<void(bool)> handle) {
            onConnectionHandle = std::move(handle);
        }

        bool MsquicSocketClient::isConnected() const {
            return connected.load();
        }

        void MsquicSocketClient::handleReceive(QUIC_STREAM_EVENT* event)
        {
            auto* rev = &event->RECEIVE;

            // =========================================================
            // 核心修复：
            // 如果 receivedBuffer 不为空，说明我们处于一个分包的中间状态。
            // 此时绝对不能把新收到的数据头当做 Packet Length 处理。
            // 必须强制进入 Fallback 逻辑进行拼接。
            // =========================================================
            bool hasPendingData = !receivedBuffer.empty();

            if (!hasPendingData) {

                // --- 优化路径：只有在没有积压数据时才尝试 ---

                // 1. 单一缓冲区优化
                if (rev->BufferCount == 1) {
                    const auto& buf = rev->Buffers[0];

                    // 至少要包含头部
                    if (buf.Length >= sizeof(int64_t)) {

                        int64_t bodyLen = 0;
                        // 使用 memcpy 安全读取，防止内存未对齐崩溃
                        std::memcpy(&bodyLen, buf.Buffer, sizeof(int64_t));

                        int64_t totalLen = sizeof(int64_t) + bodyLen;
                        constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024;

                        // 校验长度合法性
                        if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                            LOG_ERROR("Parsed packet length invalid (FastPath): %lld. Disconnecting.", bodyLen);
                            clear();
                            return;
                        }

                        // 如果包含完整包
                        if (buf.Length >= static_cast<uint64_t>(totalLen)) {
                            // 零拷贝直接解析
                            std::string_view jsonStr(
                                reinterpret_cast<const char*>(buf.Buffer + sizeof(int64_t)),
                                static_cast<size_t>(bodyLen)
                            );

                            try {
                                auto json = boost::json::parse(jsonStr).as_object();
                                if (onDataReceivedHandle) {
                                    onDataReceivedHandle(json);
                                }
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("JSON parse error: %s", e.what());
                            }

                            // 如果缓冲区里还有多余的数据（粘包：Packet A + Packet B的部分）
                            // 将剩余部分放入 receivedBuffer 供下次处理
                            if (buf.Length > static_cast<uint64_t>(totalLen)) {
                                receivedBuffer.assign(
                                    buf.Buffer + totalLen,
                                    buf.Buffer + buf.Length
                                );
                                // 尝试解析剩余部分（防止一次来了两个完整包）
                                tryParse();
                            }
                            return; // 优化路径成功处理，直接返回
                        }
                    }
                }
                // 2. 多个缓冲区但数据头完整在第一个缓冲区 (Scatter/Gather)
                else if (rev->BufferCount > 1) {
                    const auto& firstBuf = rev->Buffers[0];

                    if (firstBuf.Length >= sizeof(int64_t)) {
                        int64_t bodyLen = 0;
                        std::memcpy(&bodyLen, firstBuf.Buffer, sizeof(int64_t));

                        int64_t totalLen = sizeof(int64_t) + bodyLen;

                        // 计算本次接收的总字节数
                        uint64_t totalBytesIncoming = 0;
                        for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                            totalBytesIncoming += rev->Buffers[i].Length;
                        }

                        // 如果本次接收的数据足以构成一个完整包
                        if (totalBytesIncoming >= static_cast<uint64_t>(totalLen)) {
                            std::string jsonStr;
                            jsonStr.reserve(bodyLen);

                            // 1. 处理第一个 Buffer (扣除头部)
                            uint64_t bytesFromFirst = 0;
                            if (firstBuf.Length > sizeof(int64_t)) {
                                bytesFromFirst = std::min(
                                    static_cast<uint64_t>(firstBuf.Length - sizeof(int64_t)),
                                    static_cast<uint64_t>(bodyLen)
                                );
                                jsonStr.append(
                                    reinterpret_cast<const char*>(firstBuf.Buffer + sizeof(int64_t)),
                                    bytesFromFirst
                                );
                            }

                            // 2. 处理后续 Buffer，直到填满 bodyLen
                            uint64_t jsonBytesCollected = bytesFromFirst;
                            for (uint32_t i = 1; i < rev->BufferCount && jsonBytesCollected < bodyLen; ++i) {
                                const auto& buf = rev->Buffers[i];
                                uint64_t needed = bodyLen - jsonBytesCollected;
                                uint64_t toCopy = std::min(static_cast<uint64_t>(buf.Length), needed);

                                jsonStr.append(reinterpret_cast<const char*>(buf.Buffer), toCopy);
                                jsonBytesCollected += toCopy;
                            }

                            try {
                                auto json = boost::json::parse(jsonStr).as_object();
                                if (onDataReceivedHandle) {
                                    onDataReceivedHandle(json);
                                }

                                // 处理剩余数据 (粘包处理)
                                // 我们已经消费了 totalLen 字节，需要把剩下的放入 receivedBuffer
                                uint64_t bytesToSkip = totalLen;
                                receivedBuffer.clear(); // 确保清空，虽然理应是空的

                                for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                                    const auto& buf = rev->Buffers[i];
                                    if (bytesToSkip >= buf.Length) {
                                        bytesToSkip -= buf.Length; // 这个Buffer被完全消费了
                                    }
                                    else {
                                        // 这个Buffer只被消费了一部分，剩下的存起来
                                        receivedBuffer.insert(
                                            receivedBuffer.end(),
                                            buf.Buffer + bytesToSkip,
                                            buf.Buffer + buf.Length
                                        );
                                        bytesToSkip = 0; // 后续所有Buffer都要完整存入
                                    }
                                }

                                if (!receivedBuffer.empty()) {
                                    tryParse();
                                }
                                return; // 优化路径成功
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("JSON parse error: %s", e.what());
                                // 解析异常这里选择不做特殊处理，直接让receivedBuffer逻辑去重试或者丢弃
                            }
                        }
                    }
                }
            }

            // =========================================================
            // 3. Fallback (慢速路径)
            // 适用于：
            // - 之前有积压数据 (!receivedBuffer.empty())
            // - 接收的数据不足一个完整包
            // - 解析失败
            // =========================================================

            // 预分配内存以减少 realloc 次数
            size_t newBytes = 0;
            for (uint32_t i = 0; i < rev->BufferCount; ++i) newBytes += rev->Buffers[i].Length;
            receivedBuffer.reserve(receivedBuffer.size() + newBytes);

            for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                const auto& buf = rev->Buffers[i];
                receivedBuffer.insert(receivedBuffer.end(), buf.Buffer, buf.Buffer + buf.Length);
            }

            tryParse();
        }

        void MsquicSocketClient::tryParse()
        {
            constexpr size_t headerSize = sizeof(int64_t);
            constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10MB

            while (true) {
                // 1. 检查头部
                if (receivedBuffer.size() < headerSize) return;

                // 2. 预读长度 (使用 memcpy 处理可能的内存未对齐)
                int64_t bodyLen = 0;
                std::memcpy(&bodyLen, receivedBuffer.data(), headerSize);

                if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                    LOG_ERROR("Parsed packet length invalid (Buffer): %lld. Disconnecting.", bodyLen);
                    clear(); // 断开连接并清空 buffer
                    return;
                }

                // 3. 检查完整包 (Header + Body)
                if (static_cast<int64_t>(receivedBuffer.size()) < headerSize + bodyLen) {
                    return; // 数据不够，等待下次
                }

                // 4. 数据完整，提取 Body
                // 这里的迭代器操作虽然稍微慢一点，但在 vector 头部删除大量数据会导致内存移动。
                // 如果追求极致性能，建议使用 std::deque<uint8_t> 或者维护一个 readIndex 偏移量。
                // 但为了逻辑兼容，这里保持 vector。

                auto bodyStart = receivedBuffer.begin() + headerSize;
                auto bodyEnd = bodyStart + bodyLen;

                // 构造 JSON 字符串
                // 注意：这里不需要先拷贝到 vector<uint8_t> payload 再转 string，直接转 string 即可
                std::string jsonString(bodyStart, bodyEnd);

                // 移除 Header + Body
                receivedBuffer.erase(receivedBuffer.begin(), bodyEnd);

                try {
                    boost::json::object json = boost::json::parse(jsonString).as_object();
                    if (onDataReceivedHandle) {
                        onDataReceivedHandle(json);
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("JSON parse error in tryParse: %s", e.what());
                }

                // 继续循环，处理粘包情况（buffer里可能还有下一个包）
            }
        }

        void MsquicSocketClient::clear() {

            if (!connected.load()) return;

            if (isClear.exchange(true)) return;

            onConnectionHandle = nullptr;

            onDataReceivedHandle = nullptr;

            // 先标记断开，防止新操作
            connected.store(false);

            receivedBuffer.clear();

            if (registration) {
                registration->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                //delete registration;
                registration = nullptr;
            }

            // 清理流
            if (stream) {
                // 使用中止标志立即关闭
                MsQuic->StreamShutdown(stream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE |
                    QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
                    QUIC_STATUS_ABORTED);
                MsQuic->StreamClose(stream);
                stream = nullptr;
            }

            if (remoteStream) {
                MsQuic->StreamShutdown(remoteStream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE |
                    QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
                    QUIC_STATUS_ABORTED);
                MsQuic->StreamClose(remoteStream);
                remoteStream = nullptr;
            }

            // 清理连接（等待一小段时间让异步操作完成）
            if (connection) {
                // 先尝试优雅关闭
                MsQuic->ConnectionShutdown(
                    connection,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
                    QUIC_STATUS_ABORTED
                );

                MsQuic->ConnectionClose(connection);

                connection = nullptr;
            }

            // 清理配置和注册
            if (configuration) {
                delete configuration;
                configuration = nullptr;
            }

        }

        // 静态连接回调函数
        QUIC_STATUS QUIC_API MsquicClientConnectionHandle(HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
            auto* client = static_cast<MsquicSocketClient*>(context);
            if (!client) {
                return QUIC_STATUS_INVALID_PARAMETER;
            }

            switch (event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                if (client->onConnectionHandle) {
                    client->onConnectionHandle(true);
                }
                break;

            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                LOG_INFO("QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE");

                client->connected.store(false);
                if (client->onConnectionHandle) {
                    client->onConnectionHandle(false);
                }
                break;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            {
                client->remoteStream = event->PEER_STREAM_STARTED.Stream;

                MsQuic->SetCallbackHandler(
                    event->PEER_STREAM_STARTED.Stream,
                    hope::quic::MsquicClientStreamHandle,   // 你的静态流回调
                    client);

                break;
            }

            default:
                break;
            }

            return QUIC_STATUS_SUCCESS;
        }

        // 静态流回调函数
        QUIC_STATUS QUIC_API MsquicClientStreamHandle(HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
            auto* client = static_cast<MsquicSocketClient*>(context);
            if (!client) {
                return QUIC_STATUS_INVALID_PARAMETER;
            }

            switch (event->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                client->handleReceive(event);
                break;

            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                if (event->SEND_COMPLETE.ClientContext) {

                    QUIC_BUFFER* buffer = static_cast<QUIC_BUFFER*>(event->SEND_COMPLETE.ClientContext);

                    if (buffer->Buffer) {

                        delete[] buffer->Buffer;

                    }

                    delete buffer;

                    buffer = nullptr;

                }
                break;

            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                break;

            default:
                break;
            }

            return QUIC_STATUS_SUCCESS;
        }

    } // namespace quic
} // namespace hope
