#include "peer_connection.h"

#include "rtc_base/logging.h"

int main() {
    webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);

    // Signaling is intentionally disconnected for now. A future HTTP/SSE
    // adapter can send candidates received by this callback to the server.
    render::PeerConnection peer_connection([](std::string, std::string, int) {});

    return peer_connection.initialize() ? 0 : 1;
}
