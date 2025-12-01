#include "DataChannelObserverImpl.h"

#include "WebRTCManager.h"

#include "Logger.h"

namespace hope {

	namespace rtc {

		DataChannelObserverImpl::DataChannelObserverImpl(WebRTCManager * manager ):manager(manager) {
		}
	
		// The data channel state have changed.
		void DataChannelObserverImpl::OnStateChange() {
		}

        void DataChannelObserverImpl::OnMessage(const webrtc::DataBuffer& buffer) {
            if (buffer.size() == 0) {
                Logger::getInstance()->warning("Received empty data channel message");
                return;
            }

            if (buffer.size() > 1024 * 1024) { // 1MB limit
                Logger::getInstance()->error("Received oversized data channel message: " + std::to_string(buffer.size()) + " bytes");
                return;
            }

            // Use thread-local storage to avoid thread safety issues
            static thread_local HCURSOR lastCursor = nullptr;

            const unsigned char* data = reinterpret_cast<const unsigned char*>(buffer.data.data());
            size_t size = buffer.size();

            // Minimum size check
            if (size < sizeof(short)) {
                Logger::getInstance()->error("Message too small to contain type");
                return;
            }

            short type = -1;
            memcpy(&type, data, sizeof(short));

            // Define the same structure as sender to ensure proper alignment
#pragma pack(push,1)
            struct CursorMessage {
                short type;
                int index;
                int width;
                int height;
                int hotX;
                int hotY;
            };
#pragma pack(pop)

            switch(type) {
            case 0: { // Cursor index message
                if (size < sizeof(CursorMessage)) {
                    Logger::getInstance()->error("Invalid cursor index message size");
                    break;
                }

                const CursorMessage* msg = reinterpret_cast<const CursorMessage*>(data);

                // CRITICAL: Validate index bounds
                if (msg->index < 0 || msg->index >= manager->cursorArray.size()) {
                    Logger::getInstance()->error("Invalid cursor index: " + std::to_string(msg->index) +
                                                 " (array size: " + std::to_string(manager->cursorArray.size()) + ")");
                    break;
                }

                // Validate dimensions
                if (msg->width <= 0 || msg->width > 256 ||
                    msg->height <= 0 || msg->height > 256) {
                    Logger::getInstance()->error("Invalid cursor dimensions: " +
                                                 std::to_string(msg->width) + "x" + std::to_string(msg->height));
                    break;
                }

                // Get cursor data
                std::vector<unsigned char>& cursorData = manager->cursorArray[msg->index];

                // Verify stored data size matches expected size
                size_t expectedSize = msg->width * msg->height * 4; // RGBA
                if (cursorData.size() != expectedSize) {
                    Logger::getInstance()->error("Stored cursor data size mismatch. Expected: " +
                                                 std::to_string(expectedSize) + ", Got: " +
                                                 std::to_string(cursorData.size()));
                    break;
                }

                // Create cursor
                HCURSOR cursor = CreateCursorFromRGBA(cursorData.data(), msg->width,
                                                      msg->height, msg->hotX, msg->hotY);
                if (cursor) {
                    // Clean up previous cursor
                    if (lastCursor) {
                        DestroyCursor(lastCursor);
                    }
                    lastCursor = CopyCursor(cursor);
                    SetSystemCursor(lastCursor, 32512);
                    DestroyCursor(cursor); // Clean up the temporary cursor
                }
                break;
            }

            case 1: { // New cursor data
                if (size < sizeof(CursorMessage)) {
                    Logger::getInstance()->error("Invalid new cursor message size");
                    break;
                }

                const CursorMessage* msg = reinterpret_cast<const CursorMessage*>(data);

                // Validate dimensions
                if (msg->width <= 0 || msg->width > 256 ||
                    msg->height <= 0 || msg->height > 256) {
                    Logger::getInstance()->error("Invalid cursor dimensions: " +
                                                 std::to_string(msg->width) + "x" + std::to_string(msg->height));
                    break;
                }

                // Validate index
                if (msg->index < 0 || msg->index > manager->cursorArray.size()) {
                    Logger::getInstance()->error("Invalid cursor index for storage: " +
                                                 std::to_string(msg->index));
                    break;
                }

                // Calculate image data size
                size_t headerSize = sizeof(CursorMessage);

                // Prevent integer underflow
                if (size <= headerSize) {
                    Logger::getInstance()->error("No cursor image data");
                    break;
                }

                size_t imageSize = size - headerSize;

                // Verify image data size
                size_t expectedSize = msg->width * msg->height * 4; // RGBA
                if (imageSize != expectedSize) {
                    Logger::getInstance()->error("Image data size mismatch. Expected: " +
                                                 std::to_string(expectedSize) + ", Got: " +
                                                 std::to_string(imageSize));
                    break;
                }

                // Store cursor data
                std::vector<unsigned char> cursorData(imageSize);
                memcpy(cursorData.data(), data + headerSize, imageSize);

                // Add or update cursor in array
                if (msg->index == manager->cursorArray.size()) {
                    manager->cursorArray.push_back(std::move(cursorData));
                } else {
                    manager->cursorArray[msg->index] = std::move(cursorData);
                }

                // Create cursor
                HCURSOR cursor = CreateCursorFromRGBA(manager->cursorArray[msg->index].data(),
                                                      msg->width, msg->height,
                                                      msg->hotX, msg->hotY);
                if (cursor) {
                    // Clean up previous cursor
                    if (lastCursor) {
                        DestroyCursor(lastCursor);
                    }
                    lastCursor = CopyCursor(cursor);
                    SetSystemCursor(lastCursor, 32512);
                    DestroyCursor(cursor); // Clean up the temporary cursor
                }
                break;
            }

            default:
                Logger::getInstance()->warning("Unknown message type: " + std::to_string(type));
                break;
            }
        }

	}

}
