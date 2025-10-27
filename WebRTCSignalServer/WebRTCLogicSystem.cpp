#include "WebRTCLogicSystem.h"
#include "WebRTCSignalData.h"
#include "WebRTCSignalManager.h"

#include <chrono>

#include "Utils.h"


namespace Hope {

    WebRTCLogicSystem::WebRTCLogicSystem()
    {
        work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(ioContext)
        );
  
    }

    void WebRTCLogicSystem::RunEventLoop() {

		auto self = shared_from_this();

        threads = std::move(std::thread([self]() {

            self->ioContext.run();

            }));
    }

    boost::asio::io_context& WebRTCLogicSystem::getIoCompletePorts()
    {
		return ioContext;
    }

    WebRTCLogicSystem::~WebRTCLogicSystem() {

        if (work) {
            work.reset();
        }

        ioContext.stop();

        if (threads.joinable()) {

            threads.join();

        }

    }

    void WebRTCLogicSystem::postMessageToQueue(std::shared_ptr<WebRTCSignalData> data) {

        boost::asio::post(ioContext, [this, data = std::move(data)]() {

             data->webrtcSignalManager->handleMessage(data->json, data->webrtcSignalSocket);

            });
      
    }
}



