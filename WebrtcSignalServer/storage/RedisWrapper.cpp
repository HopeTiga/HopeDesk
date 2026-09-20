#include "RedisWrapper.h"

#include <boost/redis/src.hpp>

namespace hope {

	namespace storage {

		RedisWrapper::RedisWrapper(boost::asio::io_context& ioContext, RedisConfig redisConfig)
			: ioContext(ioContext)
			, redisConfig(std::move(redisConfig))
			, connection(std::make_unique<boost::redis::connection>(ioContext, makeRedisSslContext(this->redisConfig), makeRedisLogger(this->redisConfig)))
		{

			boost::redis::config boostRedisConfig = makeBoostRedisConfig(this->redisConfig);

			boost::redis::connection* connectionPointer = connection.get();

			boost::asio::co_spawn(ioContext, [connectionPointer, boostRedisConfig]()mutable -> boost::asio::awaitable<void> {

				co_await connectionPointer->async_run(boostRedisConfig, boost::asio::use_awaitable);

				}, CompletionHandle{});

		}

		RedisWrapper::~RedisWrapper() {

			if (connection) {

				connection->cancel();

			}

		}

		RedisWrapper::RedisWrapper(RedisWrapper && redisWrapper)
			: ioContext(redisWrapper.ioContext)
			, redisConfig(std::move(redisWrapper.redisConfig))
			, connection(std::move(redisWrapper.connection))
		{

		}

		boost::redis::connection& RedisWrapper::getRedisConnection() {

			return *connection;

		}

		boost::asio::io_context& RedisWrapper::getIoContext()
		{
			return ioContext;
		}

	}

}
