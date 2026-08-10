#pragma once

#include <memory>
#include <string>
#include <vector>

#include <boost/beast/http.hpp>

#include <absl/functional/any_invocable.h>

namespace hope {

	namespace signal {

		class HttpSocket;

		class HttpFilters
		{

		public:

			void addRule(std::string pathPattern);

			void addFilter(absl::AnyInvocable<bool(std::shared_ptr<HttpSocket>, const boost::beast::http::request<boost::beast::http::string_body>&)> filter);

			bool authorization(std::shared_ptr<HttpSocket> httpSocket, const boost::beast::http::request<boost::beast::http::string_body>& httpRequest);

		private:

			bool matchPath(const std::string& pattern, const std::string& target);

		private:

			std::vector<std::string> httpFilterRules;

			std::vector<absl::AnyInvocable<bool(std::shared_ptr<HttpSocket>, const boost::beast::http::request<boost::beast::http::string_body>&)>> httpFilters;

		};

	}

}
