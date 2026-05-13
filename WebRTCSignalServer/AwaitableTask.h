#pragma once
#include <functional>
#include <optional>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "concurrentqueue.h"

namespace hope {
    namespace core {

        using AwaitableTask = std::function<boost::asio::awaitable<void>()>;

        class TaskChannel {
        public:
            explicit TaskChannel(boost::asio::io_context& ctx, size_t maxCapacity = 65536)
                : channel(ctx.get_executor()), maxCapacity(maxCapacity) {
            }

            bool enqueue(AwaitableTask task) {
                if (queueSize.fetch_add(1) >= static_cast<ptrdiff_t>(maxCapacity)) {
                    queueSize.fetch_sub(1);
                    return false;
                }
                queue.enqueue(std::move(task));
                boost::system::error_code ec;
                channel.try_send(ec);
                return true;
            }

            boost::asio::awaitable<std::optional<AwaitableTask>> dequeue() {
                while (true) {
                    AwaitableTask task;
                    if (queue.try_dequeue(task)) {
                        queueSize.fetch_sub(1);
                        co_return std::move(task);
                    }
                    boost::system::error_code ec;
                    co_await channel.async_receive(
                        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec == boost::asio::experimental::error::channel_closed) {
                        while (queue.try_dequeue(task)) {
                            queueSize.fetch_sub(1);
                        }
                        co_return std::nullopt;
                    }
                }
            }

            void close() {
                channel.close();
            }

            bool isOpen() const {
                return channel.is_open();
            }

        private:
            boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> channel;
            moodycamel::ConcurrentQueue<AwaitableTask> queue;
            std::atomic<ptrdiff_t> queueSize{ 0 };
            size_t maxCapacity;
        };

    } // namespace core
} // namespace hope