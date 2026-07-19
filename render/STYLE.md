# C++ style guide

This project uses a compact, modern C++ naming scheme. It is intentionally
independent of the naming conventions used by WebRTC or Google.

## Naming

| Element | Rule | Example |
| --- | --- | --- |
| Namespace | `snake_case` | `render`, `media_pipeline` |
| Class, struct, union | `PascalCase` | `PeerConnection`, `EncodedFrame` |
| Type alias | `PascalCase` | `LocalIceCandidateHandler` |
| Concept | `PascalCase` | `FrameSource` |
| Function and method | `snake_case` | `initialize()`, `push_frame()` |
| Local and global variable | `snake_case` | `frame_count`, `signaling_thread` |
| Parameter | `snake_case` | `sdp_mid` |
| Data member | `snake_case_` | `peer_connection_` |
| Constant and `constexpr` value | `snake_case` | `default_timeout` |
| Scoped enum value | `snake_case` | `ConnectionState::connected` |
| Template parameter | `PascalCase` | `Frame`, `Allocator` |
| Macro | `UPPER_SNAKE_CASE` | `RENDER_ENABLE_TRACING` |
| Source file | `snake_case` | `peer_connection.cpp` |

Treat abbreviations as words: use `SdpOffer`, `HttpClient`, `sdp_offer`, and
`http_client`, not `SDPOffer` or `HTTPClient`.

Functions that perform work should normally start with a verb. Predicates
should read naturally and do not need an `is_` prefix when the adjective is
already clear, for example `initialized()` and `empty()`.

Avoid Hungarian notation, type prefixes, and leading underscores. Prefer
scoped `enum class` declarations over unscoped enums.

## External APIs

Names required by an external interface keep the spelling of that interface.
For example, WebRTC observer overrides remain `OnIceCandidate()` even though
project-owned methods use `snake_case`. Third-party types and enum values also
retain their upstream names.

## Application

`.clang-format` controls layout. Naming is kept consistent through this guide
and code review; the project does not require a separate naming linter.
