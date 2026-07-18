#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "api/create_modular_peer_connection_factory.h"
#include "rtc_base/logging.h"

int main(int argc, char** argv) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);

    auto deps = webrtc::PeerConnectionFactoryDependencies{};
    auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));

    auto device = MTL::CreateSystemDefaultDevice();

    if (device != nullptr) {
        device->release();
    }

    return 0;
}
