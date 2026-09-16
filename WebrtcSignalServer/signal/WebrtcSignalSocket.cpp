
#include "WebrtcSignalSocket.h"

#include <boost/url.hpp>
#include <string_view>
#include <cstring>
#include <array>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <ylt/struct_pack.hpp>

#include "../ssl/Ssl.h"
#include "WebrtcSignalServer.h"
#include "WebrtcSignalManager.h"
#include "WebrtcSignalPacket.h"

#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        WebrtcSignalSocket::WebrtcSignalSocket(boost::asio::io_context& ioContext, WebrtcSignalManager * webrtcSignalManager, int maxTlsHandShakeTime)
            : ioContext(ioContext)
            , resolver(ioContext)
#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)
            , webSocket(ioContext)
#else
            , webSocket(ioContext, getSslContext())
#endif
            , webrtcSignalManager(webrtcSignalManager)
            , asioConcurrentQueue(ioContext.get_executor())
            , handshakeTimeout(maxTlsHandShakeTime) {

            boost::uuids::random_generator gen;

            sessionId = boost::uuids::to_string(gen());

            receiveBuffer.resize(receiveBufferInitialSize);

            receiveFrameRanges.reserve(256);

            frameHeaderScratch.resize(maximumFramesPerWrite * maximumFrameHeaderSize);

        }

        WebrtcSignalSocket::~WebrtcSignalSocket() {

            closeBoot();

            LOG_INFO("~WebrtcSignalSocket");

        }

        std::string WebrtcSignalSocket::getSessionId() {

            return sessionId;

        }

        boost::asio::ip::tcp::socket& WebrtcSignalSocket::getSocket() {

#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)

            return webSocket.next_layer();

#else

            return webSocket.next_layer().next_layer();

#endif

        }

        boost::asio::io_context& WebrtcSignalSocket::getIoCompletionPorts() {

            return ioContext;

        }

        WebrtcSignalSocket::WebSocketStream& WebrtcSignalSocket::getWebSocket() {

            return webSocket;

        }

        boost::asio::awaitable<bool> WebrtcSignalSocket::handShake() {

            boost::beast::flat_buffer buffer;

            boost::beast::http::request<boost::beast::http::string_body> req;

            try {

#if !defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)

                co_await webSocket.next_layer().async_handshake(
                    boost::asio::ssl::stream_base::server,
                    boost::asio::cancel_after(handshakeTimeout, boost::asio::use_awaitable));

#endif

                co_await boost::beast::http::async_read(webSocket.next_layer(), buffer, req,
                    boost::asio::cancel_after(handshakeTimeout, boost::asio::use_awaitable));

                std::string accountId;

                auto authIt = req.find(boost::beast::http::field::authorization);

                if (authIt != req.end()) {

                    accountId = std::string(authIt->value());

                }
                else {

                    auto target = req.target();

                    auto parsed = boost::urls::parse_origin_form(boost::core::string_view(target.data(), target.size()));

                    if (parsed) {

                        auto it = parsed->params().find("authorization");

                        if (it != parsed->params().end()) {

                            auto v = (*it).value;

                            accountId.assign(v.data(), v.size());

                        }

                    }

                }

                if (accountId.empty()) {

                    LOG_WARN("WebrtcSignalSocket Handshake Rejected: Missing Authorization (Expect Authorization Header OR ? Authorization = Query)");

                    boost::beast::http::response<boost::beast::http::string_body> res{ boost::beast::http::status::unauthorized, req.version() };

                    res.set(boost::beast::http::field::server, "WebrtcSignalServer");

                    res.set(boost::beast::http::field::content_type, "application/json");

                    res.keep_alive(false);

                    res.body() = R"json({"state":401,"message":"Handshake Rejected: Missing Authorization (Expect Authorization Header OR ?Authorization=Query)"})json";

                    res.prepare_payload();

                    co_await boost::beast::http::async_write(webSocket.next_layer(), res, boost::asio::cancel_after(handshakeTimeout, boost::asio::use_awaitable));

                    closeSocket();

                    co_return false;

                }

                co_await webSocket.async_accept(req, boost::asio::use_awaitable);

                webSocket.binary(true);

                setTcpKeepAlive(getSocket());

                buffer.consume(buffer.size());

                setAccountId(accountId);

                webrtcSignalManager->registerSocket(accountId, shared_from_this());

                LOG_INFO("User Register Successful (HandShake): {} (ChannelIndex: {})", accountId.c_str(), webrtcSignalManager->getChannelIndex());

            }
            catch (const boost::system::system_error& se) {

                LOG_ERROR("WebrtcSignalSocket Handshake Ffailed! ERROR: {}", se.what());

                closeSocket();

                co_return false;

            }

            co_return true;
        }

        void WebrtcSignalSocket::asyncBoot() {

            if (asyncBoots.exchange(true)) return;

            boost::asio::co_spawn(ioContext, [this]()->boost::asio::awaitable<void> {

                co_await reviceCoroutine();

                co_return;

                }, [self = shared_from_this()](std::exception_ptr p) {
                    if (p) {

                        self->closeBoot();

                        try {

                            std::rethrow_exception(p);

                        }
                        catch (std::exception& e) {

                            if (self->onDisConnectHandle && !self->isHandleDisConnect.exchange(true)) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("ReviceCoroutine Error: {}", e.what());

                        }
                        catch (...) {

                            if (self->onDisConnectHandle && !self->isHandleDisConnect.exchange(true)) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("ReviceCoroutine Error: Unknown");

                        }
                    }
                    });

                boost::asio::co_spawn(ioContext, [this]()->boost::asio::awaitable<void> {

                    co_await writerCoroutine();

                    co_return;

                    }, [self = shared_from_this()](std::exception_ptr p) {
                        if (p) {
                            try {

                                std::rethrow_exception(p);

                            }
                            catch (std::exception& e) {

                                LOG_ERROR("WriterCoroutine Error: {}", e.what());

                            }
                            catch (...) {

                                LOG_ERROR("WriterCoroutine Error: Unknown");

                            }
                        }
                        });

                webSocket.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                    boost::beast::role_type::server));

        }

        void WebrtcSignalSocket::closeBoot() {

            if (!asyncBoots.exchange(false)) {

                return;

            }

            asioConcurrentQueue.close();

            closeSocket();

        }

        void WebrtcSignalSocket::closeSocket() {

            boost::system::error_code ec;

            boost::asio::ip::tcp::socket& tcpSocket = getSocket();

            if (tcpSocket.is_open()) {

                tcpSocket.cancel(ec);

                if (ec) {
                    LOG_ERROR("Cancel Failed: {}", ec.message().c_str());
                }

                // Force RST close: skip graceful TCP FIN handshake, send RST immediately
                boost::asio::detail::socket_option::linger<SOL_SOCKET, SO_LINGER> lingerOption(true, 0);

                tcpSocket.set_option(lingerOption, ec);

                if (ec) {
                    LOG_ERROR("Set SO_LINGER Failed: {}", ec.message().c_str());
                }

                tcpSocket.close(ec);

                if (ec && ec != boost::asio::error::not_connected) {

                    LOG_ERROR("Force Close Failed: {}", ec.message().c_str());

                }

                LOG_INFO("WebrtcSignalSocket Is Immediately Force Closed (RST) and Resources Are Freed");

            }

        }

        FrameBatch WebrtcSignalSocket::takeFrames(const char* data, std::size_t size, std::vector<FrameRange>& frames) {

            FrameBatch frameBatch;

            frames.clear();

            std::size_t offset = 0;

            while (size - offset >= 2) {

                const unsigned char firstByte = static_cast<unsigned char>(data[offset]);

                const unsigned char secondByte = static_cast<unsigned char>(data[offset + 1]);

                const bool masked = (secondByte & 0x80) != 0;

                std::uint64_t payloadLength = secondByte & 0x7F;

                std::size_t headerSize = 2;

                if (payloadLength == 126) {

                    if (size - offset < 4) { break; }

                    payloadLength = (static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + 2])) << 8)
                        | static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + 3]));

                    headerSize = 4;

                }
                else if (payloadLength == 127) {

                    if (size - offset < 10) { break; }

                    payloadLength = 0;

                    for (std::size_t index = 0; index < 8; ++index) {

                        payloadLength = (payloadLength << 8)
                            | static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + 2 + index]));

                    }

                    headerSize = 10;

                }

                const std::size_t maskSize = masked ? 4 : 0;

                const std::size_t wholeFrameSize = headerSize + maskSize + static_cast<std::size_t>(payloadLength);

                if (size - offset < wholeFrameSize) { break; }

                const unsigned char opcode = firstByte & 0x0F;

                if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {

                    FrameRange frameRange;

                    frameRange.offset = offset + headerSize + maskSize;

                    frameRange.length = static_cast<std::size_t>(payloadLength);

                    frameRange.masked = masked;

                    frameRange.maskOffset = offset + headerSize;

                    frameRange.final = (firstByte & 0x80) != 0;

                    frames.push_back(frameRange);

                }
                else if (opcode == 0x8) {

                    frameBatch.consumed = offset + wholeFrameSize;

                    frameBatch.closed = true;

                    break;

                }

                offset += wholeFrameSize;

                frameBatch.consumed = offset;

            }

            return frameBatch;

        }

        std::size_t WebrtcSignalSocket::encodeFrameHeader(char* out, std::size_t length, bool binary) {

            std::size_t written = 0;

            out[written++] = static_cast<char>(binary ? 0x82 : 0x81);

            if (length < 126) {

                out[written++] = static_cast<char>(length);

            }
            else if (length <= 0xFFFF) {

                out[written++] = static_cast<char>(126);

                out[written++] = static_cast<char>((length >> 8) & 0xFF);

                out[written++] = static_cast<char>(length & 0xFF);

            }
            else {

                out[written++] = static_cast<char>(127);

                for (int shift = 56; shift >= 0; shift -= 8) {

                    out[written++] = static_cast<char>((static_cast<std::uint64_t>(length) >> shift) & 0xFF);

                }

            }

            return written;

        }

        void WebrtcSignalSocket::unmaskPayload(char* payload, std::size_t length, const char* maskKey) {

            for (std::size_t index = 0; index < length; ++index) {

                payload[index] = static_cast<char>(payload[index] ^ maskKey[index % 4]);

            }

        }

        boost::asio::awaitable<void> WebrtcSignalSocket::reviceCoroutine() {

            std::string fragmentedPayload;

            bool assemblingFragment = false;

            while (asyncBoots.load()) {

                if (receiveHeldBytes == receiveBuffer.size()) {

                    if (receiveBuffer.size() >= receiveBufferMaximumSize) {

                        LOG_ERROR("WebrtcSignalSocket Frame Larger Than The Maximum Receive Buffer: {} bytes", receiveBufferMaximumSize);

                        throw std::runtime_error("Frame Larger Than The Maximum Receive Buffer");

                    }

                    std::size_t grownSize = receiveBuffer.size() * 2;

                    if (grownSize > receiveBufferMaximumSize) {

                        grownSize = receiveBufferMaximumSize;

                    }

                    receiveBuffer.resize(grownSize);

                }

                const std::size_t receivedBytes = co_await webSocket.next_layer().async_read_some(
                    boost::asio::buffer(receiveBuffer.data() + receiveHeldBytes, receiveBuffer.size() - receiveHeldBytes),
                    boost::asio::use_awaitable);

                const std::size_t totalBytes = receiveHeldBytes + receivedBytes;

                const FrameBatch frameBatch = takeFrames(receiveBuffer.data(), totalBytes, receiveFrameRanges);

                for (const FrameRange & frameRange : receiveFrameRanges) {

                    if (frameRange.masked) {

                        unmaskPayload(receiveBuffer.data() + frameRange.offset, frameRange.length,
                            receiveBuffer.data() + frameRange.maskOffset);

                    }

                    // 单条消息的上限，这里必须卡，而且必须**所有帧都卡**：
                    //   - 分片路径不受接收缓冲区上限约束（RFC 6455 允许一条消息拆成任意多帧，
                    //     每帧独立过完整性检查，N 个 FIN=0 的帧累加就能把内存吃干）；
                    //   - 不分片的单帧更不受约束 —— 缓冲区开多大，单帧上限就自动变成多大。
                    // 不分片时 fragmentedPayload 恒为空，所以这一句同时管住两条路。
                    if (fragmentedPayload.size() + frameRange.length > maximumMessageSize) {

                        LOG_ERROR("WebrtcSignalSocket Message Larger Than The Maximum Message Size: {} bytes", maximumMessageSize);

                        throw std::runtime_error("Message Larger Than The Maximum Message Size");

                    }

                    if (!frameRange.final) {

                        fragmentedPayload.append(receiveBuffer.data() + frameRange.offset, frameRange.length);

                        assemblingFragment = true;

                        continue;

                    }

                    WebrtcSignalPacket webrtcSignalPakcet(shared_from_this(), webrtcSignalManager, webrtcSignalManager->getChannelIndex());

                    if (assemblingFragment) {

                        fragmentedPayload.append(receiveBuffer.data() + frameRange.offset, frameRange.length);

                        webrtcSignalPakcet.packet = std::move(fragmentedPayload);

                        fragmentedPayload.clear();

                        assemblingFragment = false;

                    }
                    else {

                        webrtcSignalPakcet.packet.assign(receiveBuffer.data() + frameRange.offset, frameRange.length);

                    }

                    std::string_view stringView(webrtcSignalPakcet.packet.data(), webrtcSignalPakcet.packet.size());

                    size_t envelopeSize = 0;

                    struct_pack::err_code deserializeError = struct_pack::deserialize_to(webrtcSignalPakcet.webrtcEnvelope, stringView, envelopeSize);

                    if (deserializeError) {

                        LOG_ERROR("StructPack Parse Error: {}", deserializeError.message().data());

                        throw std::runtime_error("StructPack Parse Error");

                    }

                    if (webrtcSignalPakcet.webrtcEnvelope.requestType == 0) {

                        LOG_ERROR("WebrtcSignalSocket Invalid Request: missing requestType");

                        throw std::runtime_error("Invalid Request: Missing RequestType");

                    }

                    webrtcSignalManager->getLogicSystem()->postTask(std::move(webrtcSignalPakcet));

                }

                receiveHeldBytes = totalBytes - frameBatch.consumed;

                if (receiveHeldBytes > 0 && frameBatch.consumed > 0) {

                    std::memmove(receiveBuffer.data(), receiveBuffer.data() + frameBatch.consumed, receiveHeldBytes);

                }

                if (frameBatch.closed) {

                    co_return;

                }

            }
        }

        boost::asio::awaitable<void> WebrtcSignalSocket::writerCoroutine() {

            std::vector<std::string> packets;

            std::vector<boost::asio::const_buffer> segments;

            packets.reserve(maximumFramesPerWrite);

            segments.reserve(maximumFramesPerWrite * 2);

            while (asyncBoots.load()) {

                packets.clear();

                std::string packet;

                if (asioConcurrentQueue.tryDequeue(packet)) {

                    packets.push_back(std::move(packet));

                    while (packets.size() < maximumFramesPerWrite && asioConcurrentQueue.tryDequeue(packet)) {

                        packets.push_back(std::move(packet));

                    }

                }
                else {

                    if (!co_await asioConcurrentQueue.awaitDequeue(packet)) {

                        break;

                    }

                    packets.push_back(std::move(packet));

                }

                segments.clear();

                for (std::size_t index = 0; index < packets.size(); ++index) {

                    char* frameHeader = frameHeaderScratch.data() + index * maximumFrameHeaderSize;

                    const std::size_t frameHeaderSize = encodeFrameHeader(frameHeader, packets[index].size(), true);

                    segments.emplace_back(frameHeader, frameHeaderSize);

                    segments.emplace_back(boost::asio::buffer(packets[index]));

                }

                co_await boost::asio::async_write(webSocket.next_layer(), segments, boost::asio::use_awaitable);

            }

            co_return;
        }

        void WebrtcSignalSocket::setTcpKeepAlive(boost::asio::ip::tcp::socket& sock, int idle, int intvl, int probes)
        {
            int fd = sock.native_handle();
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&on), sizeof(on));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(__linux__)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
#elif defined(_WIN32)
            struct tcp_keepalive kalive {};
            kalive.onoff = 1;
            kalive.keepalivetime = idle * 1000;   // ms
            kalive.keepaliveinterval = intvl * 1000;   // ms
            DWORD bytes_returned = 0;
            WSAIoctl(fd, SIO_KEEPALIVE_VALS,
                &kalive, sizeof(kalive),
                nullptr, 0, &bytes_returned, nullptr, nullptr);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#else
#warning "Unsupported platform, TCP keep-alive parameters not tuned"
#endif
        }

        void WebrtcSignalSocket::asyncWrite(std::string packet) {

            asioConcurrentQueue.enqueue(std::move(packet));

        }

        void WebrtcSignalSocket::setOnDisConnectHandle(absl::AnyInvocable<void(std::string, std::string)>&& handle) {
            this->onDisConnectHandle = std::move(handle);
        }

        void WebrtcSignalSocket::setAccountId(const std::string& accountId) { this->accountId = accountId; }

        std::string WebrtcSignalSocket::getAccountId() { return this->accountId; }

        std::string WebrtcSignalSocket::getRemoteAddress()
        {
            return getSocket().remote_endpoint().address().to_string();
        }
    } // namespace socket
} // namespace hope