#include "signaling_client.h"

#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

#include "httplib.h"
#include "rtc_base/logging.h"

namespace render {
namespace {

constexpr std::string_view offer_path = "/api/signaling/render/offer";
constexpr std::string_view ice_path = "/api/signaling/render/ice";
constexpr std::string_view events_path = "/api/signaling/render/events";

std::string escape_json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 16);

    for (const auto character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                escaped += "\\u00";
                escaped += hex[(static_cast<unsigned char>(character) >> 4) & 0x0f];
                escaped += hex[static_cast<unsigned char>(character) & 0x0f];
            } else {
                escaped += character;
            }
        }
    }

    return escaped;
}

std::optional<std::string> json_string_field(std::string_view json, std::string_view field) {
    const auto key = '"' + std::string(field) + '"';
    auto position = json.find(key);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }

    position = json.find(':', position + key.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }

    do {
        ++position;
    } while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0);

    if (position >= json.size() || json[position] != '"') {
        return std::nullopt;
    }

    ++position;
    std::string value;
    while (position < json.size()) {
        const auto character = json[position++];
        if (character == '"') {
            return value;
        }
        if (character != '\\') {
            value += character;
            continue;
        }
        if (position >= json.size()) {
            return std::nullopt;
        }

        switch (json[position++]) {
        case '"':
            value += '"';
            break;
        case '\\':
            value += '\\';
            break;
        case '/':
            value += '/';
            break;
        case 'b':
            value += '\b';
            break;
        case 'f':
            value += '\f';
            break;
        case 'n':
            value += '\n';
            break;
        case 'r':
            value += '\r';
            break;
        case 't':
            value += '\t';
            break;
        default:
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::string json_body(std::string_view name, std::string_view value) {
    return "{\"" + std::string(name) + "\":\"" + escape_json_string(value) + "\"}";
}

std::string nullable_json_string(const std::optional<std::string>& value) {
    return value ? "\"" + escape_json_string(*value) + "\"" : "null";
}

std::string ice_json_body(const IceCandidateMessage& ice) {
    return "{\"candidate\":\"" + escape_json_string(ice.candidate) +
           "\",\"sdpMid\":" + nullable_json_string(ice.sdp_mid) +
           ",\"sdpMLineIndex\":" + (ice.sdp_mline_index ? std::to_string(*ice.sdp_mline_index) : "null") +
           ",\"usernameFragment\":" + nullable_json_string(ice.username_fragment) + "}";
}

std::optional<int> json_integer_field(std::string_view json, std::string_view field) {
    const auto key = '"' + std::string(field) + '"';
    auto position = json.find(key);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }

    position = json.find(':', position + key.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    do {
        ++position;
    } while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0);

    auto value = 0;
    const auto result = std::from_chars(json.data() + position, json.data() + json.size(), value);
    return result.ec == std::errc{} ? std::optional(value) : std::nullopt;
}

} // namespace

class SignalingClient::Impl final {
public:
    explicit Impl(std::string server_origin)
        : event_http_(server_origin), post_http_(std::move(server_origin)),
          event_stream_(event_http_, std::string(events_path)) {
        event_stream_.on_open([] { RTC_LOG(LS_INFO) << "Signaling SSE connected"; })
            .on_event("signal", [this](const httplib::sse::SSEMessage& message) { handle_event(message); })
            .on_error([](httplib::Error error) {
                RTC_LOG(LS_WARNING) << "Signaling SSE error: " << httplib::to_string(error);
            });
    }

    ~Impl() { stop(); }

    void start(SignalHandler on_answer, IceCandidateHandler on_ice_candidate) {
        on_answer_ = std::move(on_answer);
        on_ice_candidate_ = std::move(on_ice_candidate);
        event_stream_.start_async();
    }

    void stop() { event_stream_.stop(); }

    bool send_offer(const std::string& sdp) { return post(offer_path, json_body("sdp", sdp)); }

    bool send_ice_candidate(const IceCandidateMessage& candidate) { return post(ice_path, ice_json_body(candidate)); }

private:
    bool post(std::string_view path, const std::string& body) {
        const auto result = post_http_.Post(std::string(path), body, "application/json");
        if (!result) {
            RTC_LOG(LS_ERROR) << "Signaling POST failed for " << path << ": " << httplib::to_string(result.error());
            return false;
        }
        if (result->status != httplib::StatusCode::NoContent_204) {
            RTC_LOG(LS_ERROR) << "Signaling POST returned HTTP " << result->status << " for " << path;
            return false;
        }
        return true;
    }

    void handle_event(const httplib::sse::SSEMessage& message) {
        const auto type = json_string_field(message.data, "type");
        const auto sender = json_string_field(message.data, "sender");
        if (!type || !sender) {
            RTC_LOG(LS_WARNING) << "Ignoring malformed signaling event " << message.id;
            return;
        }
        if (*sender != "viewer") {
            RTC_LOG(LS_WARNING) << "Ignoring signaling event from unexpected sender: " << *sender;
            return;
        }

        if (*type == "answer") {
            const auto sdp = json_string_field(message.data, "sdp");
            if (!sdp) {
                RTC_LOG(LS_WARNING) << "Ignoring answer event without SDP: " << message.id;
                return;
            }
            on_answer_(*sdp);
        } else if (*type == "ice") {
            const auto candidate = json_string_field(message.data, "candidate");
            if (!candidate) {
                RTC_LOG(LS_WARNING) << "Ignoring ICE event without a candidate: " << message.id;
                return;
            }
            on_ice_candidate_(IceCandidateMessage{
                .candidate = *candidate,
                .sdp_mid = json_string_field(message.data, "sdpMid"),
                .sdp_mline_index = json_integer_field(message.data, "sdpMLineIndex"),
                .username_fragment = json_string_field(message.data, "usernameFragment"),
            });
        } else {
            RTC_LOG(LS_WARNING) << "Ignoring unexpected signaling event type: " << *type;
        }
    }

    httplib::Client event_http_;
    httplib::Client post_http_;
    httplib::sse::SSEClient event_stream_;
    SignalHandler on_answer_;
    IceCandidateHandler on_ice_candidate_;
};

SignalingClient::SignalingClient(std::string server_origin) : impl_(std::make_unique<Impl>(std::move(server_origin))) {}

SignalingClient::~SignalingClient() = default;

void SignalingClient::start(SignalHandler on_answer, IceCandidateHandler on_ice_candidate) {
    impl_->start(std::move(on_answer), std::move(on_ice_candidate));
}

void SignalingClient::stop() {
    impl_->stop();
}

bool SignalingClient::send_offer(const std::string& sdp) {
    return impl_->send_offer(sdp);
}

bool SignalingClient::send_ice_candidate(const IceCandidateMessage& candidate) {
    return impl_->send_ice_candidate(candidate);
}

} // namespace render
