#include "MsquicSocket.h"
#include "MsquicManager.h"
#include "MsquicData.h"

#include "MsQuicApi.h"

#include "Utils.h"

#include <boost/json.hpp>
#include <boost/asio/co_spawn.hpp>

namespace hope {

    namespace quic {

        MsquicSocket::MsquicSocket(HQUIC connection, MsquicManager* msquicManager, boost::asio::io_context& ioContext) 
            :connection(connection)
            , msquicManager(msquicManager)
            , ioContext(ioContext)
            , registrationTimer(ioContext)
            , stream(nullptr)
            , remoteStream(nullptr)
        {
      
        }

        MsquicSocket::~MsquicSocket()
        {

            registrationTimer.cancel();

            clear();

        }

        void MsquicSocket::clear()
        {
     
            if (isShutDown.exchange(true)) return;

            receivedBuffer.clear();

            if (stream) {
                MsQuic->StreamShutdown(stream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
                    0);
            }

            if (remoteStream) {
                MsQuic->StreamShutdown(remoteStream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE,
                    0);
            }

         
            if (connection) {
                MsQuic->ConnectionShutdown(
                    connection,
                    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
                    QUIC_STATUS_ABORTED
                );
            }

        }

        void MsquicSocket::runEventLoop()
        {
           stream = createStream();

           if (!stream) clear();

           boost::asio::co_spawn(ioContext, [self = shared_from_this()]()->boost::asio::awaitable<void> {
            
                co_await self->registrationTimeout();

            }, boost::asio::detached);
        }


        void MsquicSocket::writeAsync(unsigned char* data, size_t size)
        {

            if (!stream || isShutDown.load()) {

                LOG_WARNING("MsquicSocket::writeAsync failed: Stream is null or closed.");

                if (data) {
                    delete[] data;
                }
                return;
            }

            QUIC_BUFFER* buffer = new QUIC_BUFFER;

            buffer->Buffer = data;

            buffer->Length = size;

            // 添加更多错误检查
            QUIC_STATUS status = MsQuic->StreamSend(
                stream,
                buffer,
                1,
                QUIC_SEND_FLAG_NONE,
                buffer);

            if (QUIC_FAILED(status)) {

                delete[] buffer->Buffer;

                delete buffer;

                return;
            }

            return;
        }

        void MsquicSocket::receiveAsync(QUIC_STREAM_EVENT* event)
        {
            auto* rev = &event->RECEIVE;

            // =========================================================
            // 关键修复：检查是否有积压数据
            // 如果 receivedBuffer 不为空，说明处于粘包中间状态，
            // 必须强制进入 Fallback 逻辑进行拼接，绝不能尝试零拷贝读取头部。
            // =========================================================
            bool hasPendingData = !receivedBuffer.empty();

            if (!hasPendingData) {

                // --- 优化路径：只有在没有积压数据时才尝试 ---

                // 1. 单一缓冲区优化
                if (rev->BufferCount == 1) {
                    const auto& buf = rev->Buffers[0];

                    if (buf.Length >= sizeof(int64_t)) {

                        int64_t bodyLen = 0;
                        // 使用 memcpy 防止内存未对齐崩溃
                        std::memcpy(&bodyLen, buf.Buffer, sizeof(int64_t));

                        int64_t totalLen = sizeof(int64_t) + bodyLen;
                        constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10MB

                        // 校验长度
                        if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                            LOG_ERROR("Parsed packet length invalid (FastPath): %lld. Disconnecting.", bodyLen);
                            // 这里根据你的逻辑决定是否断开连接，通常应该断开
                            // close(); 
                            return;
                        }

                        if (buf.Length >= static_cast<uint64_t>(totalLen)) {
                            // 零拷贝直接解析
                            std::string_view jsonStr(
                                reinterpret_cast<const char*>(buf.Buffer + sizeof(int64_t)),
                                static_cast<size_t>(bodyLen)
                            );

                            try {
                                auto json = boost::json::parse(jsonStr).as_object();
                                auto msquicData = std::make_shared<MsquicData>(
                                    json, shared_from_this(), msquicManager);
                                msquicManager->getMsquicLogicSystem()->postTaskAsync(std::move(msquicData));
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("JSON parse error: %s", e.what());
                            }

                            // 处理剩余数据（粘包：Buffer里包含Packet A完整数据 + Packet B的部分数据）
                            if (buf.Length > static_cast<uint64_t>(totalLen)) {
                                receivedBuffer.assign(
                                    buf.Buffer + totalLen,
                                    buf.Buffer + buf.Length
                                );
                                tryParse(); // 尝试处理剩余部分
                            }
                            return; // 成功处理，返回
                        }
                    }
                }
                // 2. 多个缓冲区但数据头完整在第一个缓冲区
                else if (rev->BufferCount > 1) {
                    const auto& firstBuf = rev->Buffers[0];

                    if (firstBuf.Length >= sizeof(int64_t)) {
                        int64_t bodyLen = 0;
                        std::memcpy(&bodyLen, firstBuf.Buffer, sizeof(int64_t));

                        int64_t totalLen = sizeof(int64_t) + bodyLen;
                        constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024;

                        if (bodyLen < 0 || bodyLen > MAX_PACKET_SIZE) {
                            // 长度非法，进入Fallback或者断开，这里让他往下走进入Fallback也行，
                            // 但最好直接报错返回。
                            LOG_ERROR("Invalid length in multi-buffer: %lld", bodyLen);
                            
                            clear();

                            return;
                        }
                        else {
                            // 计算总长度
                            uint64_t totalBytesIncoming = 0;
                            for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                                totalBytesIncoming += rev->Buffers[i].Length;
                            }

                            // 如果本次接收的数据足以构成一个完整包
                            if (totalBytesIncoming >= static_cast<uint64_t>(totalLen)) {
                                std::string jsonStr;
                                jsonStr.reserve(bodyLen);

                                // 提取数据拼接字符串

                                // 1. 第一个缓冲区（跳过头部）
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

                                // 2. 后续缓冲区
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
                                    auto msquicData = std::make_shared<MsquicData>(
                                        json, shared_from_this(), msquicManager);
                                    msquicManager->getMsquicLogicSystem()->postTaskAsync(std::move(msquicData));

                                    // 处理剩余数据
                                    uint64_t consumedBytes = totalLen;
                                    receivedBuffer.clear(); // 确保是空的

                                    for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                                        const auto& buf = rev->Buffers[i];
                                        if (consumedBytes >= buf.Length) {
                                            consumedBytes -= buf.Length;
                                        }
                                        else {
                                            // 剩余部分存入 receivedBuffer
                                            receivedBuffer.insert(
                                                receivedBuffer.end(),
                                                buf.Buffer + consumedBytes,
                                                buf.Buffer + buf.Length
                                            );
                                            consumedBytes = 0;
                                        }
                                    }

                                    if (!receivedBuffer.empty()) {
                                        tryParse();
                                    }
                                    return;  // 零拷贝处理完毕
                                }
                                catch (const std::exception& e) {
                                    LOG_ERROR("JSON parse error: %s", e.what());
                                    // 解析失败，让它继续往下走到Fallback
                                }
                            }
                        }
                    }
                }
            }

            // 预分配以优化性能
            size_t newBytes = 0;
            for (uint32_t i = 0; i < rev->BufferCount; ++i) newBytes += rev->Buffers[i].Length;
            receivedBuffer.reserve(receivedBuffer.size() + newBytes);

            for (uint32_t i = 0; i < rev->BufferCount; ++i) {
                const auto& buf = rev->Buffers[i];
                receivedBuffer.insert(receivedBuffer.end(), buf.Buffer, buf.Buffer + buf.Length);
            }

            tryParse();
        }

        void MsquicSocket::tryParse()
        {
            constexpr size_t headerSize = sizeof(int64_t);
            constexpr int64_t MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10MB

            while (true) {
                // 1. 检查头部
                if (receivedBuffer.size() < headerSize) return;

                // 2. 预读长度（使用 memcpy 安全读取）
                int64_t len = 0;
                std::memcpy(&len, receivedBuffer.data(), headerSize);

                // 3. 安全检查：防止非法长度导致崩溃或OOM
                if (len < 0 || len > MAX_PACKET_SIZE) {
                    LOG_ERROR("Parsed packet length invalid in tryParse: %lld. Clearing buffer.", len);
                    // 这里遇到了严重的数据错乱，最好的办法是断开连接。
                    // 如果不断开，必须清空 buffer，否则死循环
                    receivedBuffer.clear();
                    return;
                }

                // 4. 检查完整包 (Header + Body)
                if (static_cast<int64_t>(receivedBuffer.size()) < headerSize + len) {
                    return; // 数据不够，等待下次
                }

                // 5. 数据完整，开始提取
                auto bodyStart = receivedBuffer.begin() + headerSize;
                auto bodyEnd = bodyStart + len;

                // 构造字符串 (避免先拷贝到 vector<uint8_t> 再转 string)
                std::string jsonString(bodyStart, bodyEnd);

                // 移除 Header + Body
                receivedBuffer.erase(receivedBuffer.begin(), bodyEnd);

                try {
                    boost::json::object json = boost::json::parse(jsonString).as_object();
                    auto msquicData = std::make_shared<MsquicData>(
                        json, shared_from_this(), msquicManager);
                    msquicManager->getMsquicLogicSystem()->postTaskAsync(std::move(msquicData));
                }
                catch (const std::exception& e) {
                    LOG_ERROR("JSON parse error in tryParse: %s", e.what());
                }

                // 继续循环，处理粘包中的下一个包
            }
        }

        boost::asio::awaitable<void> MsquicSocket::registrationTimeout() {

            using namespace std::chrono_literals;

            // 1. 设置 10 秒超时
            registrationTimer.expires_after(10s);

            // 2. 异步等待计时器或被取消
            boost::system::error_code ec;

            co_await registrationTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 3. 检查是否超时
            if (ec == boost::asio::error::operation_aborted) {
                // 计时器被取消 (说明在 10s 内完成了注册)
                co_return;
            }

            // 4. 超时发生，且尚未注册，则关闭连接
            if (!isRegistered.load()) {
                this->clear();

            }

            co_return;
        }


		HQUIC MsquicSocket::createStream()
		{
            HQUIC stream = nullptr;
            QUIC_STATUS status = MsQuic->StreamOpen(
                connection,
                QUIC_STREAM_OPEN_FLAG_NONE,
                MsquicSocketHandle,
                this,
                &stream);

            if (QUIC_FAILED(status)) {
                return nullptr;
            }

            MsQuic->SetCallbackHandler(
                stream,
                MsquicSocketHandle,   // 你的静态流回调
                this);

            status = MsQuic->StreamStart(
                stream,
                QUIC_STREAM_START_FLAG_IMMEDIATE | QUIC_STREAM_START_FLAG_INDICATE_PEER_ACCEPT | QUIC_STREAM_START_FLAG_PRIORITY_WORK);

            if (QUIC_FAILED(status)) {
                MsQuic->StreamClose(stream);
                return nullptr;
            }

            return stream;

		}


        void MsquicSocket::setAccountId(const std::string& accountId) { 
        
            this->accountId = accountId;

        }

        std::string& MsquicSocket::getAccountId() { 
        
            return this->accountId;

        }

        void MsquicSocket::setRegistered(bool registered) {
            
            this->isRegistered.store(registered);

            if (isRegistered.load()) {
            
                registrationTimer.cancel();

            }

        }

        bool MsquicSocket::getRegistered() {
            return this->isRegistered ;
        }

        MsquicManager* MsquicSocket::getMsquicManager()
        {
            return msquicManager;
        }

        void MsquicSocket::setRemoteStream(HQUIC remoteStream) {
        
			this->remoteStream = remoteStream;

        }

        boost::asio::io_context& MsquicSocket::getIoCompletionPorts()
        {
            return this->ioContext;
        }

        SocketType MsquicSocket::getType() {

            return SocketType::MsquicSocket;

        }

        HQUIC MsquicSocket::getConnection() {
        
			return this->connection;

        }

        void MsquicSocket::setConnection(HQUIC conn) {
        
			this->connection = conn;

        }

        void MsquicSocket::tryRelease()
        {
            if (this->connection == nullptr && this->stream == nullptr && this->remoteStream == nullptr) {
            
				boost::asio::post(ioContext, [self = shared_from_this()]() {
                
                    self->msquicManager->removeConnection(self->accountId);

					});

            }

        }


        // Stream callback
        QUIC_STATUS QUIC_API MsquicSocketHandle(
            HQUIC stream,
            void* context,
            QUIC_STREAM_EVENT* event) {

            MsquicSocket * msquicSocket = static_cast<MsquicSocket*>(context);

            if (msquicSocket == nullptr || event == nullptr) {
                return QUIC_STATUS_INVALID_PARAMETER;
            }

            switch (event->Type) {
            case QUIC_STREAM_EVENT_START_COMPLETE:
            {
                break;
            }
           

            case QUIC_STREAM_EVENT_RECEIVE:
            {

                msquicSocket->receiveAsync(event);

                break;
            }
         

            // Add handler for QUIC_STREAM_EVENT_SEND_COMPLETE (type 2)
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
            {
                if (event->SEND_COMPLETE.ClientContext) {
                    
                    QUIC_BUFFER*  buffer = static_cast<QUIC_BUFFER*>(event->SEND_COMPLETE.ClientContext);

                    if (buffer->Buffer) {
                    
						delete[] buffer->Buffer;

                    }

                    delete buffer;

                }
                break;
            }
    

            // Add handler for QUIC_STREAM_EVENT_PEER_SEND_ABORTED (type 6)
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            {
                break;

            }
   

            case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
                break;
            }
                                           
            case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
            {
                break;
            }
    

            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
             
                MsQuic->StreamClose(stream);

                if (msquicSocket) {

                    if (msquicSocket->stream == stream) {

                        msquicSocket->stream = nullptr;

                    }
                    if (msquicSocket->remoteStream == stream) {

                        msquicSocket->remoteStream = nullptr;

                    }

                    msquicSocket->tryRelease();

                }

                break;
            }
            case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
            {
                break;
            }
        

            default:
            {
                break;
            }
            }

            return QUIC_STATUS_SUCCESS;
        }

	}
}