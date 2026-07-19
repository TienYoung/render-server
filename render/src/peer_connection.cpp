#include "peer_connection.h"

#include <string_view>
#include <utility>

#include "api/create_modular_peer_connection_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/logging.h"

namespace render {
namespace {

class CreateOfferObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    using SuccessHandler = std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>)>;

    explicit CreateOfferObserver(SuccessHandler on_success) : on_success_(std::move(on_success)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
        on_success_(std::unique_ptr<webrtc::SessionDescriptionInterface>(description));
    }

    void OnFailure(webrtc::RTCError error) override {
        RTC_LOG(LS_ERROR) << "Failed to create offer: " << error.message();
    }

private:
    SuccessHandler on_success_;
};

class SetLocalDescriptionObserver : public webrtc::SetLocalDescriptionObserverInterface {
public:
    using CompletionHandler = std::function<void(webrtc::RTCError)>;

    explicit SetLocalDescriptionObserver(CompletionHandler on_complete) : on_complete_(std::move(on_complete)) {}

    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override { on_complete_(std::move(error)); }

private:
    CompletionHandler on_complete_;
};

class SetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    using CompletionHandler = std::function<void(webrtc::RTCError)>;

    explicit SetRemoteDescriptionObserver(CompletionHandler on_complete) : on_complete_(std::move(on_complete)) {}

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override { on_complete_(std::move(error)); }

private:
    CompletionHandler on_complete_;
};

} // namespace

class PeerConnection::DataChannelLogger final : public webrtc::DataChannelObserver {
public:
    explicit DataChannelLogger(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel)
        : data_channel_(std::move(data_channel)), label_(data_channel_->label()) {
        data_channel_->RegisterObserver(this);
    }

    ~DataChannelLogger() override { data_channel_->UnregisterObserver(); }

    void OnStateChange() override {
        RTC_LOG(LS_INFO) << "Data channel " << label_
                         << " state changed: " << webrtc::DataChannelInterface::DataStateString(data_channel_->state());
    }

    void OnMessage(const webrtc::DataBuffer& buffer) override {
        if (!buffer.binary) {
            const auto message = std::string_view(buffer.data.data<char>(), buffer.data.size());
            RTC_LOG(LS_INFO) << "Data channel " << label_ << " received text: " << message;
            return;
        }

        constexpr char hex_digits[] = "0123456789abcdef";
        auto message = std::string{};
        message.reserve(buffer.data.size() * 2);
        for (const auto byte : buffer.data) {
            message += hex_digits[byte >> 4];
            message += hex_digits[byte & 0x0f];
        }
        RTC_LOG(LS_INFO) << "Data channel " << label_ << " received binary: " << message;
    }

private:
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
    std::string label_;
};

PeerConnection::PeerConnection(LocalDescriptionHandler on_local_offer, LocalIceCandidateHandler on_local_ice_candidate)
    : on_local_offer_(std::move(on_local_offer)), on_local_ice_candidate_(std::move(on_local_ice_candidate)) {}

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
    auto peer_dependencies = webrtc::PeerConnectionDependencies(this);
    auto result = factory_->CreatePeerConnectionOrError(configuration, std::move(peer_dependencies));
    if (!result.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to create PeerConnection: " << result.error().message();
        factory_ = nullptr;
        return false;
    }

    peer_connection_ = result.MoveValue();

    auto data_channel = peer_connection_->CreateDataChannelOrError("render-control", nullptr);
    if (!data_channel.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to create the bootstrap data channel: " << data_channel.error().message();
        close();
        return false;
    }
    observe_data_channel(data_channel.MoveValue());

    RTC_LOG(LS_INFO) << "PeerConnection initialized with a bootstrap data channel";
    return true;
}

bool PeerConnection::create_offer() {
    if (!peer_connection_) {
        return false;
    }

    const auto observer = webrtc::make_ref_counted<CreateOfferObserver>(
        [this](std::unique_ptr<webrtc::SessionDescriptionInterface> description) {
            std::string sdp;
            if (!description->ToString(&sdp)) {
                RTC_LOG(LS_ERROR) << "Failed to serialize the local offer";
                return;
            }

            const auto set_observer = webrtc::make_ref_counted<SetLocalDescriptionObserver>(
                [this, sdp = std::move(sdp)](webrtc::RTCError error) mutable {
                    if (!error.ok()) {
                        RTC_LOG(LS_ERROR) << "Failed to apply the local offer: " << error.message();
                        return;
                    }
                    RTC_LOG(LS_INFO) << "Local offer applied";
                    on_local_offer_(std::move(sdp));
                });
            peer_connection_->SetLocalDescription(std::move(description), set_observer);
        });

    peer_connection_->CreateOffer(observer.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
    return true;
}

bool PeerConnection::set_remote_answer(const std::string& sdp) {
    if (!peer_connection_) {
        return false;
    }

    auto parse_error = webrtc::SdpParseError{};
    auto description = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &parse_error);
    if (!description) {
        RTC_LOG(LS_ERROR) << "Failed to parse the remote answer: " << parse_error.description;
        return false;
    }

    const auto observer = webrtc::make_ref_counted<SetRemoteDescriptionObserver>([this](webrtc::RTCError error) {
        if (!error.ok()) {
            RTC_LOG(LS_ERROR) << "Failed to apply the remote answer: " << error.message();
            return;
        }

        remote_description_ready_ = true;
        RTC_LOG(LS_INFO) << "Remote answer applied";
        apply_pending_remote_ice_candidates();
    });
    peer_connection_->SetRemoteDescription(std::move(description), observer);
    return true;
}

bool PeerConnection::add_remote_ice_candidate(IceCandidateMessage candidate) {
    if (!peer_connection_) {
        return false;
    }

    if (candidate.candidate.empty()) {
        RTC_LOG(LS_INFO) << "Remote ICE gathering complete";
        return true;
    }

    if (!remote_description_ready_) {
        pending_remote_ice_candidates_.push_back(std::move(candidate));
        RTC_LOG(LS_INFO) << "Remote ICE candidate queued until the answer is applied";
        return true;
    }

    return apply_remote_ice_candidate(candidate);
}

bool PeerConnection::apply_remote_ice_candidate(const IceCandidateMessage& candidate_message) {
    if (!candidate_message.sdp_mid && !candidate_message.sdp_mline_index) {
        RTC_LOG(LS_ERROR) << "Remote ICE candidate has neither sdpMid nor sdpMLineIndex";
        return false;
    }

    auto parse_error = webrtc::SdpParseError{};
    auto candidate = webrtc::IceCandidate::Create(candidate_message.sdp_mid.value_or(""),
                                                  candidate_message.sdp_mline_index.value_or(-1),
                                                  candidate_message.candidate,
                                                  &parse_error);
    if (!candidate) {
        RTC_LOG(LS_ERROR) << "Failed to parse the remote ICE candidate: " << parse_error.description;
        return false;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
        RTC_LOG(LS_ERROR) << "Failed to apply the remote ICE candidate";
        return false;
    }

    RTC_LOG(LS_INFO) << "Remote ICE candidate applied";
    return true;
}

void PeerConnection::apply_pending_remote_ice_candidates() {
    auto pending_candidates = std::move(pending_remote_ice_candidates_);
    pending_remote_ice_candidates_.clear();

    for (const auto& candidate : pending_candidates) {
        if (!apply_remote_ice_candidate(candidate)) {
            RTC_LOG(LS_ERROR) << "Failed to apply a queued remote ICE candidate";
        }
    }
}

void PeerConnection::observe_data_channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    data_channel_loggers_.push_back(std::make_unique<DataChannelLogger>(std::move(data_channel)));
}

void PeerConnection::close() {
    pending_remote_ice_candidates_.clear();
    remote_description_ready_ = false;
    data_channel_loggers_.clear();
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
    observe_data_channel(std::move(data_channel));
}

void PeerConnection::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    RTC_LOG(LS_INFO) << "ICE gathering state changed: " << webrtc::PeerConnectionInterface::AsString(new_state);
    if (new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete && on_local_ice_candidate_) {
        on_local_ice_candidate_(IceCandidateMessage{});
    }
}

void PeerConnection::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (on_local_ice_candidate_) {
        const auto& username_fragment = candidate->candidate().username();
        on_local_ice_candidate_(IceCandidateMessage{
            .candidate = candidate->ToString(),
            .sdp_mid = candidate->sdp_mid().empty() ? std::nullopt : std::optional<std::string>(candidate->sdp_mid()),
            .sdp_mline_index =
                candidate->sdp_mline_index() < 0 ? std::nullopt : std::optional<int>(candidate->sdp_mline_index()),
            .username_fragment =
                username_fragment.empty() ? std::nullopt : std::optional<std::string>(username_fragment),
        });
    }
}

void PeerConnection::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    RTC_LOG(LS_INFO) << "PeerConnection state changed: " << webrtc::PeerConnectionInterface::AsString(new_state);
}

} // namespace render
