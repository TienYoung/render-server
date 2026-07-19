#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "signaling_message.h"

namespace render {

class PeerConnection final : public webrtc::PeerConnectionObserver {
public:
    using LocalDescriptionHandler = std::function<void(std::string sdp)>;
    using LocalIceCandidateHandler = std::function<void(IceCandidateMessage candidate)>;

    PeerConnection(LocalDescriptionHandler on_local_offer, LocalIceCandidateHandler on_local_ice_candidate);
    ~PeerConnection() override;

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    bool initialize();
    bool create_offer();
    bool set_remote_answer(const std::string& sdp);
    bool add_remote_ice_candidate(IceCandidateMessage candidate);
    void close();
    [[nodiscard]] bool initialized() const;

private:
    class DataChannelLogger;

    bool apply_remote_ice_candidate(const IceCandidateMessage& candidate);
    void apply_pending_remote_ice_candidates();
    void observe_data_channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel);

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

    LocalDescriptionHandler on_local_offer_;
    LocalIceCandidateHandler on_local_ice_candidate_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    std::vector<std::unique_ptr<DataChannelLogger>> data_channel_loggers_;
    std::vector<IceCandidateMessage> pending_remote_ice_candidates_;
    bool remote_description_ready_ = false;
};

} // namespace render
