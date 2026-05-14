#pragma once
#include <map>
#include <optional>
#include <string>
#include <glaze/glaze.hpp>

struct WebRTCSignalRequest {

    std::optional<int64_t> requestType;

    std::map<std::string, glz::raw_json> dynamicData;

    std::string getString(const std::string& key) const {
        auto it = dynamicData.find(key);
        if (it == dynamicData.end()) return {};
        const auto& s = it->second.str;
        if (s.size() >= 2 && s.front() == '\"' && s.back() == '\"') return s.substr(1, s.size() - 2);
        return s;
    }

    int64_t getInt(const std::string& key) const {
        auto it = dynamicData.find(key);
        if (it == dynamicData.end()) return 0;
        return std::stoll(it->second.str);
    }

    void setInt(const std::string& key, int64_t val) {
        dynamicData[key] = glz::raw_json{ std::to_string(val) };
    }

    void setString(const std::string& key, const std::string& val) {
        dynamicData[key] = glz::raw_json{ "\"" + val + "\"" };
    }
};

template <>
struct glz::meta<WebRTCSignalRequest> {

    using T = WebRTCSignalRequest;

    static constexpr auto value = object(
        "requestType", &T::requestType
    );

    static constexpr auto unknown_read = &T::dynamicData;

    static constexpr auto unknown_write = &T::dynamicData;

};

