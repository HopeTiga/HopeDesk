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

                T val;

                if (semaphore.try_acquire()) {
                    // 既然凭证扣减成功，队列里必定有数据（排除 close 的情况）
                    if (queue.try_dequeue(val)) {
                        co_return val;
                    }
                    else {
                        // 拿不到说明是 close 塞入的结束信号
                        if (isClose.load(std::memory_order_acquire)) {
                            semaphore.release();
                            co_return std::nullopt;
                        }
                    }
                }

                co_await semaphore.async_acquire(boost::asio::use_awaitable);

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

            void reset() {

                T val;

                while (queue.try_dequeue(val)) {

                }

                isClose.store(false);

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
