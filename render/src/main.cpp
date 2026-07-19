#include <csignal>
#include <string>

#include "peer_connection.h"
#include "signaling_client.h"

#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);

    const auto server_origin = argc > 1 ? std::string(argv[1]) : "http://localhost:5040";
    render::SignalingClient signaling_client(server_origin);

    render::PeerConnection peer_connection(
        [&signaling_client](std::string offer) {
            if (!signaling_client.send_offer(offer)) {
                RTC_LOG(LS_ERROR) << "Failed to publish the local offer";
            }
        },
        [&signaling_client](render::IceCandidateMessage candidate) {
            if (!signaling_client.send_ice_candidate(candidate)) {
                RTC_LOG(LS_ERROR) << "Failed to publish a local ICE candidate";
            }
        });

    if (!peer_connection.initialize()) {
        return 1;
    }

    auto* signaling_thread = webrtc::Thread::Current();
    signaling_client.start(
        [signaling_thread, &peer_connection](std::string answer) {
            signaling_thread->PostTask(
                [&peer_connection, answer = std::move(answer)] { peer_connection.set_remote_answer(answer); });
        },
        [signaling_thread, &peer_connection](render::IceCandidateMessage candidate) {
            signaling_thread->PostTask([&peer_connection, candidate = std::move(candidate)] {
                peer_connection.add_remote_ice_candidate(candidate);
            });
        });

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    if (!peer_connection.create_offer()) {
        RTC_LOG(LS_ERROR) << "Failed to start offer creation";
        signaling_client.stop();
        return 1;
    }

    RTC_LOG(LS_INFO) << "Render signaling started against " << server_origin;
    while (stop_requested == 0 && signaling_thread->ProcessMessages(100)) {}

    signaling_client.stop();
    peer_connection.close();
    return 0;
}
