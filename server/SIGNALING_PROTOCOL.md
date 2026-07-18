# Render-to-Viewer Signaling Protocol

## Overview

This protocol coordinates one Render peer with one Viewer peer. The roles are fixed and asymmetric:

| Role | Sends over HTTP | Receives over SSE |
| --- | --- | --- |
| Render | Offer and ICE candidates | Viewer answer and ICE candidates |
| Viewer | Answer and ICE candidates | Render offer and ICE candidates |

The protocol uses ordinary HTTP requests for outbound messages and Server-Sent Events (SSE) for inbound messages. It does not use WebSocket. SDP descriptions and ICE candidates are treated as opaque strings by the signaling server.

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
  "candidate": "candidate:render-1 1 udp ..."
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
  "candidate": "candidate:viewer-1 1 udp ..."
}
```

A successful request returns `204 No Content`. The candidate is delivered to the Render SSE stream.

## SSE Event Format

Both SSE endpoints use the same wire format:

```text
id: 12
event: signal
data: {"sequence":12,"type":"ice","sender":"render","payload":"candidate:render-1 1 udp ...","createdAt":"2026-07-18T12:00:00+00:00"}

```

The `data` field is a JSON object with these properties:

| Property | Type | Description |
| --- | --- | --- |
| `sequence` | integer | Monotonically increasing server event identifier |
| `type` | string | `offer`, `answer`, or `ice` |
| `sender` | string | `render` or `viewer` |
| `payload` | string | SDP text or an ICE candidate |
| `createdAt` | string | UTC timestamp in ISO 8601 format |

The SSE event name is always `signal`.

## Negotiation Sequence

1. Render opens `GET /api/signaling/render/events` and keeps the SSE connection open.
2. Viewer opens `GET /api/signaling/viewer/events` and keeps the SSE connection open.
3. Render sends an offer with `POST /api/signaling/render/offer`.
4. Viewer receives the `offer` event from its SSE stream.
5. Viewer sends an answer with `POST /api/signaling/viewer/answer`.
6. Render receives the `answer` event from its SSE stream.
7. Render and Viewer send ICE candidates through their respective HTTP endpoints.
8. Each role receives only the other role's ICE candidates from its own SSE stream.

The two SSE connections may be opened before or after messages are posted. Buffered events are replayed when a client connects.

## Reconnection and Replay

Each SSE message includes an `id` equal to its `sequence` value. Standard SSE clients such as browser `EventSource` automatically send the most recently received ID in the `Last-Event-ID` header when reconnecting.

The server resumes after that event ID and does not intentionally resend older events. A client may also provide an explicit query parameter:

```http
GET /api/signaling/viewer/events?after=12
```

When both are present, the `after` query parameter takes precedence over `Last-Event-ID`.

The server sends this SSE retry directive when a connection is established:

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
- The server transports signaling text only; it does not create or manage WebRTC peer connections.
