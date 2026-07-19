# Render-to-Viewer Signaling Protocol

## Overview

This protocol coordinates one Render peer with one Viewer peer. The roles are fixed and asymmetric:

| Role | Sends over HTTP | Receives over SSE |
| --- | --- | --- |
| Render | Offer and ICE candidates | Viewer answer and ICE candidates |
| Viewer | Answer and ICE candidates | Render offer and ICE candidates |

The protocol uses ordinary HTTP requests for outbound messages and Server-Sent Events (SSE) for inbound messages. It does not use WebSocket. SDP descriptions and ICE candidate fields are transported without interpretation by the signaling server. The server produces SSE responses with the .NET 10 `TypedResults.ServerSentEvents` API and represents events as `SseItem<SignalEvent>` values.

The Viewer implementation creates a real `RTCPeerConnection` and a local data channel named `viewer-data`. Render offers must include an `application` media section for an SCTP data channel. Render may also create its own data channels; the Viewer accepts them through the browser's `datachannel` event.

All endpoints are relative to the server origin and use the `/api/signaling` prefix.

## Endpoints

### Render endpoints

#### Send an offer

```http
POST /api/signaling/render/offer
Content-Type: application/json

{
  "sdp": "v=0\r\n..."
}
```

A successful request returns `204 No Content`.

Sending an offer starts a new negotiation. Because the current implementation supports only one Render-to-Viewer pair, it clears buffered events from the previous negotiation before storing the new offer.

#### Send an ICE candidate

```http
POST /api/signaling/render/ice
Content-Type: application/json

{
  "candidate": "candidate:render-1 1 udp ...",
  "sdpMid": "0",
  "sdpMLineIndex": 0,
  "usernameFragment": "render-ufrag"
}
```

A successful request returns `204 No Content`. The candidate is delivered to the Viewer SSE stream.

#### Receive Viewer events

```http
GET /api/signaling/render/events
Accept: text/event-stream
```

This SSE stream delivers only:

- Viewer answers
- Viewer ICE candidates

### Viewer endpoints

#### Receive Render events

```http
GET /api/signaling/viewer/events
Accept: text/event-stream
```

This SSE stream delivers only:

- Render offers
- Render ICE candidates

#### Send an answer

```http
POST /api/signaling/viewer/answer
Content-Type: application/json

{
  "sdp": "v=0\r\n..."
}
```

A successful request returns `204 No Content`. The answer is delivered to the Render SSE stream.

#### Send an ICE candidate

```http
POST /api/signaling/viewer/ice
Content-Type: application/json

{
  "candidate": "candidate:viewer-1 1 udp ...",
  "sdpMid": "0",
  "sdpMLineIndex": 0,
  "usernameFragment": "viewer-ufrag"
}
```

A successful request returns `204 No Content`. The candidate is delivered to the Render SSE stream.

ICE request objects follow the relevant fields from `RTCIceCandidateInit`:

| Property | Type | Required | Description |
| --- | --- | --- | --- |
| `candidate` | string | Yes | ICE candidate attribute without the `a=` prefix |
| `sdpMid` | string or null | Yes | Media section identifier associated with the candidate |
| `sdpMLineIndex` | integer or null | Yes | Zero-based media section index associated with the candidate |
| `usernameFragment` | string or null | Yes | ICE username fragment, when exposed by the WebRTC implementation |

An empty `candidate` string signals end-of-candidates. Its other fields may be `null`:

```json
{
  "candidate": "",
  "sdpMid": null,
  "sdpMLineIndex": null,
  "usernameFragment": null
}
```

## SSE Event Format

Both SSE endpoints use the same wire format:

```text
id: 12
event: signal
data: {"sequence":12,"type":"ice","sender":"render","ice":{"candidate":"candidate:render-1 1 udp ...","sdpMid":"0","sdpMLineIndex":0,"usernameFragment":"render-ufrag"},"createdAt":"2026-07-18T12:00:00+00:00"}

```

The `data` field is a JSON object with these properties:

| Property | Type | Description |
| --- | --- | --- |
| `sequence` | integer | Monotonically increasing server event identifier |
| `type` | string | `offer`, `answer`, or `ice` |
| `sender` | string | `render` or `viewer` |
| `sdp` | string | Present only for `offer` and `answer` events |
| `ice` | object | Present only for `ice` events; contains the complete ICE request object |
| `createdAt` | string | UTC timestamp in ISO 8601 format |

The SSE event name is always `signal`.

## Negotiation Sequence

1. Render opens `GET /api/signaling/render/events` and keeps the SSE connection open.
2. Viewer opens `GET /api/signaling/viewer/events` and keeps the SSE connection open.
3. Render creates a peer connection and at least one data channel before creating its offer. This ensures the offer contains an SCTP `application` media section.
4. Render sends the offer with `POST /api/signaling/render/offer`.
5. Viewer receives the `offer` event from its SSE stream and applies it as the remote description.
6. Viewer creates an answer, applies it as the local description, and sends it with `POST /api/signaling/viewer/answer`.
7. Render receives the `answer` event from its SSE stream and applies it as the remote description.
8. Render and Viewer send complete ICE candidate objects through their respective HTTP endpoints.
9. Each role receives only the other role's ICE candidates from its own SSE stream and passes them to `RTCPeerConnection.addIceCandidate()`.
10. The data channel becomes usable after the ICE/DTLS/SCTP connection is established and its `open` event fires.

The two SSE connections may be opened before or after messages are posted. Buffered events are replayed when a client connects.

Trickle ICE can arrive before a peer has applied the remote SDP description. Each peer must queue such candidates and call `addIceCandidate()` only after `setRemoteDescription()` succeeds. The bundled Viewer implements this queue.

## Reconnection and Replay

Each SSE message includes an `id` equal to its `sequence` value. Standard SSE clients such as browser `EventSource` automatically send the most recently received ID in the `Last-Event-ID` header when reconnecting.

The server resumes after that event ID and does not intentionally resend older events. A client may also provide an explicit query parameter:

```http
GET /api/signaling/viewer/events?after=12
```

When both are present, the `after` query parameter takes precedence over `Last-Event-ID`.

Each emitted event includes this SSE retry directive:

```text
retry: 1000
```

It asks compatible clients to wait approximately one second before reconnecting.

## Current Constraints

- Only one Render peer and one Viewer peer are supported.
- There are no room IDs, peer IDs, or multi-party routing rules.
- A new Render offer replaces the buffered state from the previous negotiation.
- Events are stored only in server memory and are lost when the process restarts.
- There is currently no authentication, authorization, payload size limit, or schema-level SDP/ICE validation.
- The server transports signaling data only; it does not create or manage WebRTC peer connections.
- The bundled Viewer uses the browser's real `RTCPeerConnection`; the Render peer must provide a compatible WebRTC implementation.
