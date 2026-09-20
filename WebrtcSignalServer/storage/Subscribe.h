#pragma once

#include <string>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/redis.hpp>

#include <absl/functional/any_invocable.h>

#include "RedisWrapper.h"

#include "RedisConfig.h"

#include "../utils/CompletionHandle.h"

namespace hope {

	namespace storage {

		using RedisMessageHandle = absl::AnyInvocable<void(std::string_view channel, std::string_view payload)>;

		class Subscribe {

		public:

			Subscribe(boost::asio::io_context & ioContext, RedisConfig redisConfig, RedisMessageHandle messageHandle);

			~Subscribe();

			Subscribe(const Subscribe &) = delete;

			Subscribe& operator=(const Subscribe &) = delete;

			Subscribe(Subscribe &&) = delete;

			Subscribe& operator=(Subscribe &&) = delete;

			boost::asio::awaitable<void> asyncSubscribe(std::string channel);

		private:

			boost::asio::awaitable<void> receiveLoop();

			RedisWrapper redisWrapper;

			boost::redis::generic_flat_response response;

			RedisMessageHandle messageHandle;

		};

	}

}
