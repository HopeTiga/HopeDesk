#include "MsquicSocketClient.h"
#include "MsQuicApi.h"
#include "Utils.h"
#include <boost/json.hpp>

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
            if (!connected.exchange(false)) return;
            if (isClear.exchange(true)) return;

            // 1. 先关掉所有回调源，确保不再进用户代码
            if (connection) {
                MsQuic->ConnectionShutdown(connection,
                                           QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                           0);
                // 等待 QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE 触发后再 close
            }
            if (stream) {
                MsQuic->StreamShutdown(stream,
                                       QUIC_STREAM_SHUTDOWN_FLAG_NONE,
                                       0);
            }
            if (remoteStream) {
                MsQuic->StreamShutdown(remoteStream,
                                       QUIC_STREAM_SHUTDOWN_FLAG_NONE,
                                       0);
            }

            // 2. 清自己的资源（此时回调不会再进来）
            onConnectionHandle = nullptr;
            onDataReceivedHandle = nullptr;
            receivedBuffer.clear();

            // 3. 最后才释放/置空
            if (registration) {
                registration->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                registration = nullptr;
            }
            if (configuration) {
                delete configuration;
                configuration = nullptr;
            }
            // connection/stream 在 SHUTDOWN_COMPLETE 里由 msquic 自动 close
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

            // =================================================================================
            // 【修复核心】：处理粘包/半包的关键逻辑
            // 如果缓存区里已经有上一波剩下的数据（哪怕只有一个字节），
            // 新来的数据必须追加到缓存区后面，不能直接当做新包头处理。
            // =================================================================================
            if (!receivedBuffer.empty()) {
                for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                    const auto& buf = rev->Buffers[i];
                    receivedBuffer.insert(receivedBuffer.end(), buf.Buffer, buf.Buffer + buf.Length);
                }
                tryParse();
                return;
            }

            // =================================================================================
            // 零拷贝优化路径 (Zero-Copy Path)
            // 只有当缓存区为空，且当前事件只有一个 Buffer 时，尝试直接解析，减少内存拷贝
            // =================================================================================
            if (rev->BufferCount == 1) {
                const auto& buf = rev->Buffers[0];

                if (buf.Length >= sizeof(int64_t)) {
                    int64_t bodyLen = 0;
                    std::memcpy(&bodyLen, buf.Buffer, sizeof(int64_t));

                    constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024;
                    if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                        LOG_ERROR("Parsed packet length invalid (FastPath): %lld. Disconnecting.", bodyLen);
                        clear();
                        return;
                    }

                    int64_t totalLen = sizeof(int64_t) + bodyLen;

                    // 情况 A: 当前 Buffer 包含了一个完整的包 (Header + Body)
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

                        // 情况 B: 粘包 (Buffer 里不仅有这个包，后面还跟着别的数据)
                        if (buf.Length > static_cast<uint64_t>(totalLen)) {
                            receivedBuffer.assign(
                                buf.Buffer + totalLen,
                                buf.Buffer + buf.Length
                            );
                            tryParse();
                        }
                        return;
                    }
                }
            }

            // =================================================================================
            // 通用路径 (Fallback Path)
            // 1. BufferCount > 1 (Scatter/Gather I/O)
            // 2. BufferCount == 1 但数据不足一个完整包
            // 3. 其他情况
            // 直接将所有数据追加到 receivedBuffer，然后尝试解析
            // =================================================================================
            size_t totalBytes = 0;
            for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                totalBytes += rev->Buffers[i].Length;
            }
            receivedBuffer.reserve(receivedBuffer.size() + totalBytes);

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
                if (receivedBuffer.size() < headerSize) return;

                int64_t bodyLen = 0;
                std::memcpy(&bodyLen, receivedBuffer.data(), headerSize);

                if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                    LOG_ERROR("Parsed packet length invalid (Buffer): %lld. Disconnecting.", bodyLen);
                    clear();
                    return;
                }

                if (static_cast<int64_t>(receivedBuffer.size()) < headerSize + bodyLen) {
                    return; // 数据不够，等待下次
                }

                // 构造 string 用于解析 (排除头部8字节)
                std::string payloadStr(
                    receivedBuffer.begin() + headerSize,
                    receivedBuffer.begin() + headerSize + bodyLen
                );

                // 在解析前先移除数据，确保无论解析是否成功，坏数据都不会卡死循环
                receivedBuffer.erase(
                    receivedBuffer.begin(),
                    receivedBuffer.begin() + headerSize + bodyLen
                );

                try {
                    auto json = boost::json::parse(payloadStr).as_object();
                    if (onDataReceivedHandle) {
                        onDataReceivedHandle(json);
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("JSON parse error in tryParse: %s", e.what());
                }
            }
        }

        void MsquicSocketClient::clear() {
            if (!connected.exchange(false)) return;
            if (isClear.exchange(true)) return;

            // 1. 关闭所有回调源，确保 msquic 不再进用户代码
            if (connection) {
                MsQuic->ConnectionShutdown(connection,
                                           QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                           0);
                connection = nullptr;
            }
            if (stream) {
                MsQuic->StreamShutdown(stream,
                                       QUIC_STREAM_SHUTDOWN_FLAG_NONE,
                                       0);
                stream = nullptr;
            }
            if (remoteStream) {
                MsQuic->StreamShutdown(remoteStream,
                                       QUIC_STREAM_SHUTDOWN_FLAG_NONE,
                                       0);
                remoteStream = nullptr;
            }

            // 2. 清上层回调和缓存
            onConnectionHandle = nullptr;
            onDataReceivedHandle = nullptr;
            receivedBuffer.clear();

            // 3. 最后释放/置空（此时回调已不可能再进来）
            if (registration) {
                registration->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                registration = nullptr;
            }
            if (configuration) {
                delete configuration;
                configuration = nullptr;
            }
            // connection/stream 指针留给 msquic 在 SHUTDOWN_COMPLETE 里置 nullptr
        }

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
                MsQuic->ConnectionClose(connection);
                if (client->onConnectionHandle) {
                    client->onConnectionHandle(false);
                }
                break;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            {
                client->remoteStream = event->PEER_STREAM_STARTED.Stream;
                MsQuic->SetCallbackHandler(
                    event->PEER_STREAM_STARTED.Stream,
                    hope::quic::MsquicClientStreamHandle,
                    client);
                break;
            }

            default:
                break;
            }

            return QUIC_STATUS_SUCCESS;
        }

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

            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:{

                MsQuic->StreamClose(stream);

                break;

            }


            default:
                break;
            }

            return QUIC_STATUS_SUCCESS;
        }

    } // namespace quic
} // namespace hope
