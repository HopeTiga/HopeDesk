#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <random>

#include <boost/container_hash/hash.hpp>

#include <boost/unordered/unordered_node_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace hope {

	inline std::uint64_t getProcessWideHashSeed() {

		static const std::uint64_t seed = [] {

			std::random_device randomDevice;

			return (static_cast<std::uint64_t>(randomDevice()) << 32) | static_cast<std::uint64_t>(randomDevice());

			}();

		return seed;
	}

	struct StringHasher {

		using is_transparent = void;

		std::size_t operator()(std::string_view value) const noexcept {

			return boost::hash<std::string_view>{}(value) ^ getProcessWideHashSeed();

		}

	};

	template <class ValueType>
	using StringKeyedNodeMap = boost::unordered_node_map<std::string, ValueType, StringHasher, std::equal_to<>>;

	template <class ValueType>
	using StringKeyedFlatMap = boost::unordered_flat_map<std::string, ValueType, StringHasher, std::equal_to<>>;

}
