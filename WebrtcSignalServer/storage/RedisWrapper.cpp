#include "RedisWrapper.h"

#include <chrono>

#include <boost/redis/src.hpp>

namespace hope {

	namespace storage {

		RedisWrapper::RedisWrapper(boost::asio::io_context& ioContext, RedisConfig redisConfig)
			: ioContext(ioContext)
			, redisConfig(std::move(redisConfig))
			, connection(std::make_shared<boost::redis::connection>(ioContext, makeRedisSslContext(this->redisConfig), makeRedisLogger(this->redisConfig)))
		{

			boost::redis::config boostRedisConfig = makeBoostRedisConfig(this->redisConfig);

			boost::asio::co_spawn(ioContext, [connection = this->connection, boostRedisConfig, redisConfig = this->redisConfig]()mutable -> boost::asio::awaitable<void> {

				for (;;) {

					boost::system::error_code runErrorCode;

					co_await connection->async_run(boostRedisConfig,
						boost::asio::redirect_error(boost::asio::deferred, runErrorCode));

					if (runErrorCode == boost::asio::error::operation_aborted) {

						co_return;

					}

					if (!redisConfig.isRestart) {

						LOG_WARN("RedisWapper AsyncRun IsReStart: {} Quit", redisConfig.isRestart);

						break;

					}

					LOG_ERROR("RedisWrapper AsyncRun Exited: {} ; Restart In 1s", runErrorCode.message());

					boost::asio::steady_timer retryTimer(co_await boost::asio::this_coro::executor);

					retryTimer.expires_after(std::chrono::seconds{ 1 });

					boost::system::error_code waitErrorCode;

					co_await retryTimer.async_wait(boost::asio::redirect_error(boost::asio::deferred, waitErrorCode));

				}

				}, CompletionHandle{});

		}

		RedisWrapper::~RedisWrapper() {

			if (connection) {

				connection->cancel();

			}

		}

		RedisWrapper::RedisWrapper(RedisWrapper&& redisWrapper)
			: ioContext(redisWrapper.ioContext)
			, redisConfig(std::move(redisWrapper.redisConfig))
			, connection(std::move(redisWrapper.connection))
		{

		}

		boost::redis::connection& RedisWrapper::getRedisConnection() {

			return *connection;

		}

		boost::asio::io_context& RedisWrapper::getIoContext() {

			return ioContext;

		}

	}

}
