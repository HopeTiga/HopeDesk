#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hope {

	namespace signal {

		class WebrtcSignalSocket;

		class WebrtcSignalManager;

		enum class RequestType : int {

			request = 1

		};

		struct WebrtcEnvelopeView {

			int requestType = 0;

			int state = 0;

			std::string_view message;

			std::string_view accountId;

			std::string_view targetId;

		};

		struct WebrtcPayloadRange {

			std::uint32_t offset = 0;

			std::uint32_t length = 0;

		};

		class WebrtcSignalPayload : public std::string {

		public:

			using std::string::operator=;

			std::vector<WebrtcPayloadRange> ranges;

			std::size_t count() const noexcept { return ranges.empty() ? 1 : ranges.size(); }

			std::string_view payload(std::size_t index = 0) const noexcept {

				if (ranges.empty()) {

					return std::string_view(data(), size());

				}

				const WebrtcPayloadRange& range = ranges[index];

				return std::string_view(data() + range.offset, range.length);

			}

		};

		class WebrtcSignalPacket {

		public:

			WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager * webrtcSignalManager, int channelIndex);

			WebrtcSignalPacket(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket& operator=(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			WebrtcSignalPacket& operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket;

			WebrtcSignalPayload packet;

			WebrtcEnvelopeView webrtcEnvelope;

			WebrtcSignalManager * webrtcSignalManager;

			int channelIndex;

		};
	}

}
