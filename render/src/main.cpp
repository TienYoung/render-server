#include "api/create_modular_peer_connection_factory.h"
#include "rtc_base/logging.h"

int main(int argc, char** argv) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);

    auto deps = webrtc::PeerConnectionFactoryDependencies{};
    auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));

    return 0;
}
