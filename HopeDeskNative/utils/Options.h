#pragma once

// 基于 Boost.JSON 的连接/配置选项容器(直接用 boost::json::object)。

#include <boost/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace utils {

class Options {
public:
    Options() = default;

    boost::json::value& operator[](std::string_view key) { return obj_[key]; }
    const boost::json::value& operator[](std::string_view key) const { return obj_.at(key); }

    bool        has(std::string_view key) const { return obj_.contains(key); }
    std::string str(std::string_view key, const std::string& def = std::string()) const {
        if (const auto* v = obj_.if_contains(key)) if (v->is_string()) return std::string(v->get_string().c_str());
        return def;
    }
    std::int64_t intVal(std::string_view key, std::int64_t def = 0) const {
        if (const auto* v = obj_.if_contains(key)) if (v->is_int64()) return v->get_int64();
        return def;
    }
    bool boolVal(std::string_view key, bool def = false) const {
        if (const auto* v = obj_.if_contains(key)) if (v->is_bool()) return v->get_bool();
        return def;
    }
    double dbl(std::string_view key, double def = 0.0) const {
        if (const auto* v = obj_.if_contains(key)) if (v->is_double()) return v->get_double();
        return def;
    }

    boost::json::object&  object(std::string_view key) { return obj_[key].emplace_object(); }
    boost::json::array&   array(std::string_view key)  { return obj_[key].emplace_array(); }

    std::string serialize() const { return boost::json::serialize(obj_); }

    static Options parse(std::string_view text, boost::system::error_code& ec) {
        Options o;
        auto v = boost::json::parse(text, ec);
        if (!ec && v.is_object()) o.obj_ = std::move(v.get_object());
        return o;
    }

    boost::json::object&       raw()       { return obj_; }
    const boost::json::object& raw() const { return obj_; }

private:
    boost::json::object obj_;
};

inline std::string serialize(const boost::json::value& v) { return boost::json::serialize(v); }
inline std::string serialize(const boost::json::object& o) { return boost::json::serialize(o); }
inline std::string serialize(const boost::json::array& a) { return boost::json::serialize(a); }

} // namespace utils
