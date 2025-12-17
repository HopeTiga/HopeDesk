#include <iostream>
#include <memory>
#include <boost/asio.hpp>

#include "ConfigManager.h"
#include "ProtectProcess.h"

int main(int argc,char * argv[])
{
    ConfigManager::Instance().Load("config.ini", ConfigManager::Format::Ini);

	boost::asio::io_context ioContext;

	std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    hope::protect::ProtectProcess protectProcess(ioContext);

    protectProcess.createProcess(ConfigManager::Instance().GetString("Protect.process"));

    boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);

    signals.async_wait([&ioContext, &workGuard,&protectProcess](const boost::system::error_code& error, int signal) {

        protectProcess.killChildProcess();

        workGuard.reset();

        ioContext.stop();

        });


	ioContext.run();

}
