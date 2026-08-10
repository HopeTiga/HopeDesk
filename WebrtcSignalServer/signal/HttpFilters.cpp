#include "HttpFilters.h"

#include <string_view>
#include <utility>

namespace hope {

    namespace signal
    {

        void HttpFilters::addRule(std::string pathPattern) {

            httpFilterRules.push_back(std::move(pathPattern));

        }

        void HttpFilters::addFilter(absl::AnyInvocable<bool(std::shared_ptr<HttpSocket>, const boost::beast::http::request<boost::beast::http::string_body>&)> filter) {

            httpFilters.push_back(std::move(filter));

        }

        bool HttpFilters::authorization(std::shared_ptr<HttpSocket> httpSocket, const boost::beast::http::request<boost::beast::http::string_body>& httpRequest) {

            for (const std::string& httpFilterRule : httpFilterRules) {

                if (matchPath(httpFilterRule, httpRequest.target())) return true;

            }

            for (absl::AnyInvocable<bool(std::shared_ptr<HttpSocket>, const boost::beast::http::request<boost::beast::http::string_body>&)>& httpFilter : httpFilters) {

                if (!httpFilter(httpSocket, httpRequest)) return false;

            }

            return true;

        }

        bool HttpFilters::matchPath(const std::string& pattern, const std::string& target) {

            if (pattern.empty() || pattern == "*") return true;

            if (pattern.back() == '*') {

                std::string_view prefix(pattern.data(), pattern.size() - 1);

                return target.compare(0, prefix.size(), prefix) == 0;

            }

            return target == pattern;

        }

    }

}
