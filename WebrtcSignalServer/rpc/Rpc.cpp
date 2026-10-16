#include "Rpc.h"

#include "../signal/WebrtcSignalServer.h"
#include "CoroRpcHandleInterface.h"
#include "CoroRpcHandleImpl.h"

void initCoroRpcHandleInterface(hope::signal::WebrtcSignalServer * webrtcSignalServer) {

    std::unique_ptr<hope::rpc::CoroRpcHandleInterface> coroRpcHandleInterface = std::make_unique<hope::rpc::CoroRpcHandleImpl>(webrtcSignalServer);

    webrtcSignalServer->registerRpcHandleImpl(std::move(coroRpcHandleInterface));

}