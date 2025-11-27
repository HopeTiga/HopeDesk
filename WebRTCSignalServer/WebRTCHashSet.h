#pragma once
#include <absl/container/flat_hash_set.h>
#include <absl/synchronization/mutex.h>
#include <optional>
#include <string>
#include <vector>

namespace hope {
    namespace utils {
        template<class Key>
        class WebRTCHashSet {
        public:  // 这里移除了多余的 {
            WebRTCHashSet() {
                flatHashSet.reserve(10240);
            }

            ~WebRTCHashSet() = default;

            WebRTCHashSet(const WebRTCHashSet& other) {
                absl::MutexLock lock(&mutex);
                absl::MutexLock lock_other(&other.mutex);
                flatHashSet = other.flatHashSet;
            }

            // 移动构造函数
            WebRTCHashSet(WebRTCHashSet&& other) noexcept {
                absl::MutexLock lock(&mutex);
                absl::MutexLock lock_other(&other.mutex);
                flatHashSet = std::move(other.flatHashSet);
            }

            // 拷贝赋值运算符
            WebRTCHashSet& operator=(const WebRTCHashSet& other) {
                if (this != &other) {
                    absl::MutexLock lock(&mutex);
                    absl::MutexLock lock_other(&other.mutex);
                    flatHashSet = other.flatHashSet;
                }
                return *this;
            }

            // 移动赋值运算符
            WebRTCHashSet& operator=(WebRTCHashSet&& other) noexcept {
                if (this != &other) {
                    absl::MutexLock lock(&mutex);
                    absl::MutexLock lock_other(&other.mutex);
                    flatHashSet = std::move(other.flatHashSet);
                }
                return *this;
            }

            // 插入元素
            void insert(const Key& key) {
                absl::MutexLock lock(&mutex);
                flatHashSet.insert(key);
            }

            // 删除元素
            void erase(const Key& key) {
                absl::MutexLock lock(&mutex);
                flatHashSet.erase(key);
            }

            // 通过迭代器删除元素
            void erase(typename absl::flat_hash_set<Key>::iterator it) {
                absl::MutexLock lock(&mutex);
                flatHashSet.erase(it);
            }

            // 清空所有元素
            void clear() {
                absl::MutexLock lock(&mutex);
                flatHashSet.clear();
            }

            // 查找元素（返回迭代器）
            typename absl::flat_hash_set<Key>::iterator find(const Key& key) {
                absl::MutexLock lock(&mutex);
                return flatHashSet.find(key);
            }

            // 查找元素（const版本）
            typename absl::flat_hash_set<Key>::const_iterator find(const Key& key) const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.find(key);
            }

            // 检查元素是否存在
            bool contains(const Key& key) const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.contains(key);
            }

            // 获取元素数量
            size_t size() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.size();
            }

            // 检查是否为空
            bool empty() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.empty();
            }

            // 批量插入元素
            template<typename InputIterator>
            void insertRange(InputIterator first, InputIterator last) {
                absl::MutexLock lock(&mutex);
                flatHashSet.insert(first, last);
            }

            // 获取所有元素的副本
            std::vector<Key> toVector() const {
                absl::MutexLock lock(&mutex);
                return std::vector<Key>(flatHashSet.begin(), flatHashSet.end());
            }

            // 新增：获取并删除一个元素（从集合中任意位置）
            std::optional<Key> pop() {
                absl::MutexLock lock(&mutex);
                if (flatHashSet.empty()) {
                    return std::nullopt;
                }
                auto it = flatHashSet.begin();
                Key value = *it;
                flatHashSet.erase(it);
                return value;
            }

            // 新增：获取并删除指定元素
            std::optional<Key> take(const Key& key) {
                absl::MutexLock lock(&mutex);
                auto it = flatHashSet.find(key);
                if (it != flatHashSet.end()) {
                    Key value = *it;
                    flatHashSet.erase(it);
                    return value;
                }
                return std::nullopt;
            }

            // 迭代器相关方法
            typename absl::flat_hash_set<Key>::iterator begin() {
                absl::MutexLock lock(&mutex);
                return flatHashSet.begin();
            }

            typename absl::flat_hash_set<Key>::iterator end() {
                absl::MutexLock lock(&mutex);
                return flatHashSet.end();
            }

            typename absl::flat_hash_set<Key>::const_iterator begin() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.begin();
            }

            typename absl::flat_hash_set<Key>::const_iterator end() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.end();
            }

            typename absl::flat_hash_set<Key>::const_iterator cbegin() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.cbegin();
            }

            typename absl::flat_hash_set<Key>::const_iterator cend() const {
                absl::MutexLock lock(&mutex);
                return flatHashSet.cend();
            }

        private:
            absl::flat_hash_set<Key> flatHashSet;
            mutable absl::Mutex mutex;
        };

    }  // namespace utils
}  // namespace hope