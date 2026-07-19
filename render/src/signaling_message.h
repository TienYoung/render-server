#pragma once

#include <optional>
#include <string>

namespace render {

struct IceCandidateMessage {
    std::string candidate;
    std::optional<std::string> sdp_mid;
    std::optional<int> sdp_mline_index;
    std::optional<std::string> username_fragment;
};

} // namespace render
