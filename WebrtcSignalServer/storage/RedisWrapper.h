#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/redis.hpp>

#include <absl/functional/any_invocable.h>

#include "../utils/CompletionHandle.h"

#include "RedisConfig.h"

namespace hope {

	namespace storage {

		class RedisWrapper {

		public:

			RedisWrapper(boost::asio::io_context & ioContext, RedisConfig redisConfig);

			~RedisWrapper();

			RedisWrapper(const RedisWrapper &) = delete;

			RedisWrapper& operator=(const RedisWrapper&) = delete;

			RedisWrapper(RedisWrapper && redisWrapper);

			RedisWrapper& operator=(RedisWrapper&& redisWrapper) = delete;

			boost::redis::connection & getRedisConnection();

			boost::asio::io_context& getIoContext();

		private:

			boost::asio::io_context& ioContext;

			RedisConfig redisConfig;

			std::shared_ptr<boost::redis::connection> connection;

		};

	}

}
