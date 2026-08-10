#include "Rpc.h"

#include "../signal/WebrtcSignalServer.h"
#include "CoroRpcHandleInterface.h"
#include "CoroRpcHandleImpl.h"

void initCoroRpcHandleInterface(std::shared_ptr<hope::signal::WebrtcSignalServer> webrtcSignalServer) {

    std::unique_ptr<hope::rpc::CoroRpcHandleInterface> coroRpcHandleInterface = std::make_unique<hope::rpc::CoroRpcHandleImpl>(*webrtcSignalServer.get());

    webrtcSignalServer->registerRpcHandleImpl(std::move(coroRpcHandleInterface));

}