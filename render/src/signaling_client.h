#pragma once

#include <functional>
#include <memory>
#include <string>

#include "signaling_message.h"

namespace render {

class SignalingClient final {
public:
    using SignalHandler = std::function<void(std::string payload)>;
    using IceCandidateHandler = std::function<void(IceCandidateMessage candidate)>;

    explicit SignalingClient(std::string server_origin);
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    void start(SignalHandler on_answer, IceCandidateHandler on_ice_candidate);
    void stop();
    [[nodiscard]] bool send_offer(const std::string& sdp);
    [[nodiscard]] bool send_ice_candidate(const IceCandidateMessage& candidate);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
