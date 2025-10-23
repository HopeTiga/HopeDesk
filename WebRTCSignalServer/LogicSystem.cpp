#include "LogicSystem.h"
#include <chrono>
#include "Utils.h"

namespace Hope {
    LogicSystem::LogicSystem(size_t size)
        :size(size)
        , isStop(false)
        , threads(size), readyVector(size), ioContexts(size), works(size), channels(size), taskChannels(size)
    {

    }

    void LogicSystem::initializeThreads() {

        for (int i = 0; i < size; i++) {

            auto work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
                boost::asio::make_work_guard(ioContexts[i])
            );

            works[i] = std::move(work);

            threads[i] = std::move(std::thread([this, i]() {
                ioContexts[i].run();
                }));

            channels[i] = std::make_unique<boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>>(ioContexts[i], 1);

            taskChannels.emplace_back(moodycamel::ConcurrentQueue<std::shared_ptr<WebRTCSignalData>>(1024));

        }

        for (int i = 0; i < size; i++) {

            auto self = shared_from_this();

            if (!self) {
                LOG_ERROR("LogicSystem shared_from_this() failed");
                break;
            }

            boost::asio::co_spawn(ioContexts[i], [self, i]() -> boost::asio::awaitable<void> {

                for (;;) {

                    std::shared_ptr<WebRTCSignalData> data = nullptr;

                    while (self->taskChannels[i].try_dequeue(data)) {

                        if (data != nullptr) {

                            data->webrtcSignalManager->handleMessage(data->json, data->webrtcSignalSocket);

                        }

                        data = nullptr;
                    }

                    if (!self->isStop && !self->readyVector[i].exchange(true)) {

                        co_await self->channels[i]->async_receive(boost::asio::use_awaitable);
                    }
                    else {

                        std::shared_ptr<WebRTCSignalData> data = nullptr;

                        while (self->taskChannels[i].try_dequeue(data)) {

                            if (data != nullptr) {

                                data->webrtcSignalManager->handleMessage(data->json, data->webrtcSignalSocket);

                            }

                            data = nullptr;

                        }

                        co_return;

                    }

                }

                co_return;

                }, [this](std::exception_ptr p) {
                    if (p) {
                        try {

                            std::rethrow_exception(p);

                        }
                        catch (const std::exception& e) {

                            LOG_ERROR("LogicSystem coroutine std::exception: %s", e.what());
                        }
                    }
                    });
        }

    }

    LogicSystem::~LogicSystem() {

        isStop = true;

        for (auto& work : works) {
            // 重置 work guard，这会让 io_context 停止运行
            if (work) {
                work.reset();
            }
        }

        // 明确停止所有 io_context
        for (auto& context : ioContexts) {
            context.stop();
        }

        for (auto& thread : threads) {

            if (thread.joinable()) {

                thread.join();

            }
        }

    }

    void LogicSystem::postMessageToQueue(std::shared_ptr<WebRTCSignalData> data, int channelIndex) {

        taskChannels[channelIndex].enqueue(data);

        if (readyVector[channelIndex].exchange(false)) {

            channels[channelIndex]->try_send(boost::system::error_code{});

        }

    }
}



