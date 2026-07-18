#include "peer_connection.h"

#include <utility>

#include "api/create_modular_peer_connection_factory.h"
#include "rtc_base/logging.h"

namespace render {

PeerConnection::PeerConnection(LocalIceCandidateHandler on_local_ice_candidate)
    : on_local_ice_candidate_(std::move(on_local_ice_candidate)) {}

PeerConnection::~PeerConnection() {
    close();
}

bool PeerConnection::initialize() {
    if (initialized()) {
        return true;
    }

    auto factory_dependencies = webrtc::PeerConnectionFactoryDependencies{};
    factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(factory_dependencies));
    if (!factory_) {
        RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
        return false;
    }

    auto configuration = webrtc::PeerConnectionInterface::RTCConfiguration{};
    configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    auto peer_dependencies = webrtc::PeerConnectionDependencies(this);
    auto result = factory_->CreatePeerConnectionOrError(configuration, std::move(peer_dependencies));
    if (!result.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to create PeerConnection: " << result.error().message();
        factory_ = nullptr;
        return false;
    }

    peer_connection_ = result.MoveValue();
    RTC_LOG(LS_INFO) << "PeerConnection initialized; signaling is not connected yet";
    return true;
}

void PeerConnection::close() {
    if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;
    }
    factory_ = nullptr;
}

bool PeerConnection::initialized() const {
    return peer_connection_ != nullptr;
}

void PeerConnection::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
    RTC_LOG(LS_INFO) << "Signaling state changed: " << webrtc::PeerConnectionInterface::AsString(new_state);
}

void PeerConnection::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    RTC_LOG(LS_INFO) << "Remote data channel received: " << data_channel->label();
}

void PeerConnection::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    RTC_LOG(LS_INFO) << "ICE gathering state changed: " << webrtc::PeerConnectionInterface::AsString(new_state);
}

void PeerConnection::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    std::string candidate_sdp;
    if (!candidate->ToString(&candidate_sdp)) {
        RTC_LOG(LS_WARNING) << "Failed to serialize local ICE candidate";
        return;
    }

    if (on_local_ice_candidate_) {
        on_local_ice_candidate_(std::move(candidate_sdp), candidate->sdp_mid(), candidate->sdp_mline_index());
    }
}

void PeerConnection::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    RTC_LOG(LS_INFO) << "PeerConnection state changed: " << webrtc::PeerConnectionInterface::AsString(new_state);
}

} // namespace render
