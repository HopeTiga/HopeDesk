#pragma once

#include <string>
#include <string_view>
#include <functional>

#include <boost/container_hash/hash.hpp>

#include <boost/unordered/unordered_node_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace hope {

	struct StringHasher {

		using is_transparent = void;

		std::size_t operator()(std::string_view value) const noexcept {

			return boost::hash<std::string_view>{}(value);

		}

	};

	template <class ValueType>
	using StringKeyedNodeMap = boost::unordered_node_map<std::string, ValueType, StringHasher, std::equal_to<>>;

	template <class ValueType>
	using StringKeyedFlatMap = boost::unordered_flat_map<std::string, ValueType, StringHasher, std::equal_to<>>;

}
