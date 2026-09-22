#include "Subscribe.h"

#include <boost/asio/redirect_error.hpp>
#include <boost/redis/push_parser.hpp>

#include "../utils/Utils.h"

namespace hope {

	namespace storage {

		Subscribe::Subscribe(boost::asio::io_context& ioContext, RedisConfig redisConfig, RedisMessageHandle messageHandle)
			: redisWrapper(ioContext, std::move(redisConfig))
			, messageHandle(std::move(messageHandle))
		{

			redisWrapper.getRedisConnection().set_receive_response(response);

			boost::asio::co_spawn(ioContext, receiveLoop(), CompletionHandle{});

		}

		Subscribe::~Subscribe() {

		}

		boost::asio::awaitable<void> Subscribe::asyncSubscribe(std::string channel) {

			boost::redis::request subscribeRequest;

			subscribeRequest.subscribe({ channel });

			co_await redisWrapper.getRedisConnection().async_exec(
				subscribeRequest, boost::redis::ignore, boost::asio::use_awaitable);

		}

		boost::asio::awaitable<void> Subscribe::receiveLoop() {

			while (true) {

				boost::system::error_code errorCode;

				co_await redisWrapper.getRedisConnection().async_receive2(
					boost::asio::redirect_error(boost::asio::use_awaitable, errorCode));

				if (errorCode) {

					LOG_ERROR("Redis Subscribe Receive Stopped: {}", errorCode.message());

					co_return;

				}

				if (!response.has_value()) {

					LOG_ERROR("Redis Subscribe Response Error, Resetting");

					response.emplace();

					continue;

				}

				const boost::redis::resp3::flat_tree& tree = response.value();

				const boost::span<const boost::redis::resp3::node_view> nodeView{ tree.data(), tree.size() };

				for (const boost::redis::push_view& message : boost::redis::push_parser(nodeView)) {

					messageHandle(message.channel, message.payload);

				}

				response.value().clear();

			}

		}

	}

}
