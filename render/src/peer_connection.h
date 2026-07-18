#pragma once

#include <functional>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

namespace render {

class PeerConnection final : public webrtc::PeerConnectionObserver {
public:
    using LocalIceCandidateHandler =
        std::function<void(std::string candidate, std::string sdp_mid, int sdp_mline_index)>;

    explicit PeerConnection(LocalIceCandidateHandler on_local_ice_candidate = {});
    ~PeerConnection() override;

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    bool initialize();
    void close();
    [[nodiscard]] bool initialized() const;

private:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

    LocalIceCandidateHandler on_local_ice_candidate_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
};

} // namespace render
