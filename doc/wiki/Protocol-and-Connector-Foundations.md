# Protocol and connector foundations

Use this page to select the appliance-side codec or session-state library for
an integration. These libraries are available for software development and
tests; the process shells do not expose live SOBP, FIX or exchange endpoints.

The embedded MockX adapter stack is a separate path documented in
[MockX and QuickFIX integration](https://github.com/lehajamb/sorx/wiki/MockX-and-QuickFIX-Integration).
The appliance FIX translator is not a replacement name for the embedded
QuickFIX session adapter.

## Select the boundary you need

| Boundary | Implemented foundation | Work still needed for deployment |
| --- | --- | --- |
| Client binary ingress | SOBP v1 encoding/decoding, session validation, credits and authentication interfaces. | Network supervisor, durable session provisioning, production MAC/KDF/key providers. |
| Client FIX compatibility | Bounded FIX 4.4 command translation and response encoding. | Socket/logon/mTLS service and session recovery. |
| Native venue order entry | Nasdaq OUCH 5.0/Soup and Cboe EDGX BOE codecs, identity planners and new/cancel encoders. | Complete session networking, drop copy, reconciliation and venue UAT/certification. |
| Native venue market data | ITCH 5.0/MoldUDP64 and EDGX PITCH decode paths with recovery-message seams. | Qualified network/recovery orchestration and exchange certification. |

## Encode and validate SOBP v1

The canonical wire format uses explicit big-endian fields, a 64-byte header
and CRC32C. The maximum payload is 88 bytes, so an unauthenticated frame is at
most 152 bytes. Message families cover logon, heartbeat, new/cancel/replace,
mass cancel, durable acknowledgement, rejection, execution and status.

For a bounded ingress implementation:

1. Construct the typed message with a matching header message type and payload.
2. Supply caller-owned storage to `sobp_v1::encode_to`; inspect its error and
   `bytes_written` before using the output.
3. Decode from the immutable receive span with `sobp_v1::decode` and reject
   invalid sizes, flags, reserved fields, CRC, text or semantics.
4. Apply the session and authentication contracts before forwarding commands
   into risk and dispatch.

The convenience `encode()` returns a vector and is intended for tools/tests;
gateways should use `encode_to()`. CRC protects framing integrity and does not
authenticate a peer.

For authenticated input, pass the complete envelope to
`SessionAuthenticator::verify`, then validate the returned message with
`SessionValidator`. The bare SOBP decoder expects the frame without an appended
MAC tag; do not feed the whole authenticated envelope to that decoder.

`SessionValidator` checks role, session ID, sequencing, logon, heartbeat and
independent normal/cancel credits. Its bounded request cache identifies
same-session retries and conflicting reuse of request IDs. Durable idempotency
must outlive that object and belongs in the order gateway/recorder integration.
A session replay result is not permission to duplicate a venue send.

## Establish authenticated sessions

Authenticate the exact logon transcript through `IAuthenticationKeyProvider`.
Successful verification produces a noncopyable, single-use
`VerifiedLogonCapability`; `SessionAuthenticator` consumes that capability.
Do not synthesize an authenticated session from identifiers alone.

The key-provider interface derives short-lived, direction-specific material
into caller-owned storage and returns its generation. It does not authorize
export of a long-lived root key. The authenticated envelope binds the SOBP
frame to session, epoch, direction, key ID and tag size; sequence validation
prevents replay. Key retirement/rotation and provider failure are explicit
error paths.

Tags are 16 or 32 bytes, making the maximum authenticated frame 184 bytes.
The MAC implementation is a supplied interface, not a bundled production
cryptographic provider. Authenticated credit exhaustion still consumes the
sequence consistently; do not reset sequence state to retry the same frame.

## Translate the supported FIX subset

The FIX 4.4 translator accepts new (`D`), cancel (`F`), replace (`G`) and mass
cancel (`q`) commands. It verifies framing, BodyLength, CheckSum, field order,
required tags, identities and supported values. Limits include 4,096 frame
bytes, 64 input fields and 256 instrument mappings.

Provide explicit session identity, account, price precision and instrument
mappings. Inspect `TranslateResult`, including `offending_tag`, instead of
forwarding partially decoded data. The fixed descriptor preserves FIX-only
time-in-force and replay details. Conversion to SOBP can reject unsupported
semantics; do not silently discard them to force a binary command.

Outbound responses use bounded fields and caller-owned buffers. The encoder
recalculates tags 9 and 10. The translator alone does not perform FIX logon,
persist sequence stores or open sockets.

## Bind native frames to recorded ownership

Register each venue's route-time wire-identity planner before routing. New
and cancel encoders use the exact planned session and native identity.
Cancellation, replay and checkpoint restart must preserve that identity;
creating a fresh one during recovery could duplicate an external order.

Native codecs use caller-owned buffers and exact integer fields. The feed
decoders emit source-sequence-ordered actions, apply transactional directory
updates, and expose bounded gap/snapshot recovery seams. See
[Market data](https://github.com/lehajamb/sorx/wiki/Market-Data) for book and
feed arbitration, and
[Appliance execution foundation](https://github.com/lehajamb/sorx/wiki/Appliance-Execution-Foundation)
for the durable authorization required before TX.

## Verify before integrating a transport

Use `sorx_appliance_protocol_tests`, `sorx_session_auth_tests`,
`sorx_fixgw_translator_tests`, `sorx_native_protocol_codec_tests` and
`sorx_native_marketdata_codec_tests`. Include malformed/truncated input,
identity mismatches, duplicate requests, credit exhaustion, key rotation,
sequence gaps and restart identity preservation. Build commands and the
baseline Linux compiler caveat are in
[Testing and troubleshooting](https://github.com/lehajamb/sorx/wiki/Testing-and-Troubleshooting).

The checked-in native codecs are uncertified. Portable codec/session tests
cannot establish exchange interoperability or production latency.

## Source baseline

- [SOBP v1 wire and session contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/protocol/sobp_v1.hpp)
- [Verified logon, key-provider and authentication contracts](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/protocol/session_auth.hpp)
- [Bounded FIX translator API and errors](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/include/sorx/fixgw/translator.hpp)
- [Native codec and restart-identity tests](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/tests/native_protocol_codec_tests.cpp)
- [Implemented connector coverage and remaining services](https://github.com/lehajamb/sorx/blob/3234de923d6497b9a119c80485a717ac24d4e6fe/docs/APPLIANCE_IMPLEMENTATION.md)
