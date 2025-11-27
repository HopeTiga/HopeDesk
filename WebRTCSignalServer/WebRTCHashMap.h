#pragma once
#include <absl/container/flat_hash_map.h>
#include <absl/synchronization/mutex.h>
#include <optional>
#include <string>

namespace hope {
    namespace utils {
        template<class Key, class Value>
        class WebRTCHashMap {
        public:

            WebRTCHashMap() {
            
                flatHashMap.reserve(10240);

            }

            ~WebRTCHashMap() = default;

            // 插入元素
            void insert(const Key& key, const Value& value) {
                absl::MutexLock lock(&mutex);
                flatHashMap.insert({ key, value });
            }

            // 删除元素
            void erase(const Key& key) {
                absl::MutexLock lock(&mutex);
                flatHashMap.erase(key);
            }

            // 通过迭代器删除元素
            void erase(typename absl::flat_hash_map<Key, Value>::iterator it) {
                absl::MutexLock lock(&mutex);
                flatHashMap.erase(it);
            }

            // 清空所有元素
            void clear() {
                absl::MutexLock lock(&mutex);
                flatHashMap.clear();
            }

            // 查找元素（返回迭代器）
            typename absl::flat_hash_map<Key, Value>::iterator find(const Key& key) {
                absl::MutexLock lock(&mutex);
                return flatHashMap.find(key);
            }

            // 查找元素（const版本）
            typename absl::flat_hash_map<Key, Value>::const_iterator find(const Key& key) const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.find(key);
            }

            // 检查元素是否存在
            bool contains(const Key& key) const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.contains(key);
            }

            // 安全获取值
            std::optional<Value> get(const Key& key) const {
                absl::MutexLock lock(&mutex);
                auto it = flatHashMap.find(key);
                if (it != flatHashMap.end()) {
                    return it->second;
                }
                return std::nullopt;
            }

            // 获取元素数量
            size_t size() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.size();
            }

            // 检查是否为空
            bool empty() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.empty();
            }

            // 直接访问操作符（注意：如果key不存在会插入默认值）
            Value& operator[](const Key& key) {
                absl::MutexLock lock(&mutex);
                return flatHashMap[key];
            }

            // 迭代器相关方法
            typename absl::flat_hash_map<Key, Value>::iterator begin() {
                absl::MutexLock lock(&mutex);
                return flatHashMap.begin();
            }

            typename absl::flat_hash_map<Key, Value>::iterator end() {
                absl::MutexLock lock(&mutex);
                return flatHashMap.end();
            }

            typename absl::flat_hash_map<Key, Value>::const_iterator begin() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.begin();
            }

            typename absl::flat_hash_map<Key, Value>::const_iterator end() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.end();
            }

            typename absl::flat_hash_map<Key, Value>::const_iterator cbegin() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.cbegin();
            }

            typename absl::flat_hash_map<Key, Value>::const_iterator cend() const {
                absl::MutexLock lock(&mutex);
                return flatHashMap.cend();
            }

        private:
            absl::flat_hash_map<Key, Value> flatHashMap;
            mutable absl::Mutex mutex;
        };
    }
}