#pragma once


#include <boost/asio.hpp>
#include <boost/sam.hpp>
#include <stdexcept>
#include <optional>
#include <atomic>
#include "../utils/concurrentqueue.h"

namespace hope {
    namespace signal {
        template<typename T>
        class AsioConcurrentQueue {
        public:
            explicit AsioConcurrentQueue(boost::asio::io_context::executor_type executor)
                : semaphore(executor, 0) {
            }

            bool tryDequeue(T& out) {

                if (semaphore.try_acquire()) {

                    if (queue.try_dequeue(out)) {

                        return true;

                    }

                    semaphore.release();

                    return false;

                }

                return false;

            }

            std::size_t tryDequeueBulk(T* out, std::size_t maximum) {

                std::size_t held = static_cast<std::size_t>(semaphore.try_acquire_many(static_cast<int>(maximum)));

                if (held == 0) {

                    return 0;

                }

                std::size_t count = queue.try_dequeue_bulk(out, held);

                for (std::size_t index = count; index < held; ++index) {

                    semaphore.release();

                }

                return count;

            }

            boost::asio::awaitable<bool> awaitDequeue(T& out) {

                co_await semaphore.async_acquire(boost::asio::use_awaitable);

                if (queue.try_dequeue(out)) {

                    co_return true;

                }

                if (isClose.load(std::memory_order_acquire)) {

                    semaphore.release();

                    out = T{};

                    co_return false;

                }

                throw std::logic_error("Queue Is Empty But Semaphore Was Acquired Without Close!");

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

                while (semaphore.try_acquire()) {

                }

                isClose.store(false);

            }

            bool enqueue(T t) {
                if (isClose.load(std::memory_order_acquire)) {
                    return false;
                }
                if (!queue.enqueue(std::move(t))) {
                    return false;
                }
                semaphore.release();

                return true;
            }

        private:
            hopeMoodycamel::ConcurrentQueue<T> queue;
            boost::sam::basic_semaphore<boost::asio::io_context::executor_type> semaphore;
            std::atomic<bool> isClose{ false };
        };
    }
}
