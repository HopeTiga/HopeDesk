#pragma once

#include <boost/asio.hpp>
#include <boost/sam.hpp>
#include <stdexcept>
#include <optional>
#include <atomic>
#include "concurrentqueue.h"

namespace hope {
    namespace core {
        template<typename T>
        class AsioConcurrentQueue {
        public:
            explicit AsioConcurrentQueue(boost::asio::any_io_executor ex)
                : semaphore(ex, 0) {
            }

            boost::asio::awaitable<std::optional<T>> dequeue() {

                co_await semaphore.async_acquire(boost::asio::use_awaitable);

                T val;

                if (queue.try_dequeue(val)) {
                    co_return val;
                }

                if (isClose.load(std::memory_order_acquire)) {
                    semaphore.release();
                    co_return std::nullopt;
                }

                throw std::logic_error("Queue is empty but semaphore was acquired without close!");
            }

            void close() {
                bool expected = false;
                if (isClose.compare_exchange_strong(expected, true, std::memory_order_release)) {
                    semaphore.release();
                }
            }

            bool enqueue(T t) {
                if (isClose.load(std::memory_order_acquire)) {
                    return false;
                }
                queue.enqueue(std::move(t));
                semaphore.release();

                return true;
            }

        private:
            moodycamel::ConcurrentQueue<T> queue;
            boost::sam::basic_semaphore<> semaphore;
            std::atomic<bool> isClose{ false };
        };
    }
}