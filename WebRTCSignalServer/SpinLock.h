#pragma once
#include <atomic>
#include <thread>

namespace hope {

	namespace utils {
	
        // 基于 atomic_flag 的极简自旋锁 (1字节开销)
        struct SpinLock {
            std::atomic_flag flag = ATOMIC_FLAG_INIT;

            void lock() {
                // 尝试获取锁
                while (flag.test_and_set(std::memory_order_acquire)) {

#if defined(__cpp_lib_atomic_wait)
            // 如果支持，进入等待状态，避免 CPU 空转 100%
                    flag.wait(true, std::memory_order_relaxed);
#else
            // 传统自旋优化：提示 CPU 这是自旋循环
                    std::this_thread::yield();
#endif
                }
            }

            void unlock() {
                flag.clear(std::memory_order_release);
#if defined(__cpp_lib_atomic_wait)
                flag.notify_one();
#endif
            }
        };

	}

}