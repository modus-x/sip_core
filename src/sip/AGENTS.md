<!-- Parent: ../AGENTS.md -->

# src/sip/ — SIP stack (PJSIP integration)

Everything PJSIP-touching lives here. The big classes are `SIPVoIPLink` (PJSIP bridge / event loop), `SIPAccount` (registration, credentials, route management), `SIPCall` (call lifecycle + media negotiation), `Sdp` (SDP offer/answer), and `SipTransport` (UDP/TCP/TLS).

## Files

| File                                | Role                                                                                                       |
|-------------------------------------|------------------------------------------------------------------------------------------------------------|
| `sipvoiplink.h/cpp`                 | **PJSIP bridge.** Owns the PJSIP endpoint, caching pool, transport broker, SIP thread (`sipThread_`). Routes incoming requests to the right `SIPAccount`. `guessAccount(user, host, fromUri)` is the demux logic. |
| `sipaccount.h/cpp`                  | **SIP account** — registration, credentials, TLS, keepalive, push, presence. Supports **main + backup service route** with OPTIONS keepalive and automatic failover. Connectivity changes drive transport rebind + re-INVITE on active calls. Large class — read the header carefully. |
| `sipaccount_config.h/cpp`           | `SipAccountConfig` — SIP-specific account YAML fields (hostname, port, credentials, srtp, service routes, registration expire, keepalive type).  |
| `sipaccountbase.h/cpp`              | Abstract base (`SIPAccountBase`) — common to any SIP-derived account; defines `newIncomingCall`, RTP port allocation, message engine bridge. |
| `sipaccountbase_config.h/cpp`       | `SipAccountBaseConfig` — shared SIP base config (transport, ICE, codecs).                                  |
| `sipcall.h/cpp`                     | **SIP call** — full state machine, media stream setup (`RtpStream`), reinvite handling, hold/offhold, transfer, DTMF over SIP INFO, video orientation, **early media (183 with SDP)**, connectivity-driven re-INVITE retry. Owns `Sdp` and `RtpSession`s. |
| `sdp.h/cpp`                         | SDP build + parse (PJMEDIA-backed). Handles offer/answer, codec negotiation, ICE candidates, RTP/RTCP-mux, attribute manipulation. |
| `sdes_negotiator.h/cpp`             | SRTP crypto suite negotiation (RFC 4568) — used by `Sdp` when SRTP is configured.                          |
| `siptransport.h/cpp`                | Transport wrapper (UDP/TCP/TLS); state listener fan-out; transport-type queries.                           |
| `sippresence.h/cpp`                 | PIDF presence: publish (PUBLISH), buddy subscription (SUBSCRIBE / NOTIFY).                                 |
| `pres_sub_client.h/cpp`             | Client-side presence subscription state machine.                                                           |
| `pres_sub_server.h/cpp`             | Server-side presence subscription state machine.                                                           |
| `sipevents.h/cpp`                   | Generic event-package framework on top of `pjsip-simple` (for non-presence events).                        |
| `custom_event_sub_client.h/cpp`     | Subscriber for application-defined event packages.                                                         |

## SIP call lifecycle (read once)

```
                +----------------+
                |  SIPVoIPLink   |  (sip_thread, PJSIP endpoint)
                +-------+--------+
                        | guessAccount(from)
                        v
              +---------+----------+
              |    SIPAccount      |  registration, credentials, route
              +---------+----------+
                        | new{Outgoing,Incoming}Call
                        v
              +---------+----------+      +-----------------+
              |     SIPCall        +----->|     Sdp         |
              | RtpStream rtpStreams_[]   | offer/answer    |
              |                    |      +-----------------+
              | onPeerRinging      |
              | onAnswered         |      +-----------------+
              | onEarlyMediaProgress183 -->|  RtpSession    |  (audio/video)
              | onMediaNegotiationComplete +-----------------+
              | hangup / refuse / transfer
              +-------+------------+
                      | emitSignal<CallSignal::StateChange> etc.
                      v
                  (host app via src/client/ring_signal.h)
```

State live on `Call` base (`CallState`, `ConnectionState`); SIP-specific transitions live in `SIPCall::onAnswered`, `onPeerRinging`, `onClosed`, `onFailure`, `onBusyHere`, `onReceiveReinvite`, `onMediaNegotiationComplete`.

## Registration & failover (the tricky bits)

`SIPAccount` implements a fairly elaborate registration/keepalive scheme — DO NOT touch it without reading the header:

- **Topologies**: `NoRoute`, `ServiceRoute`, `ServiceRouteWithBackup` (`resolveKeepAliveTopology`). Backup route lets the account fail over to a secondary registrar.
- **Keepalive types**: SIP OPTIONS (TCP/TLS or UDP when configured) or raw UDP packets.
- **Fast probes**: `enableMainRouteFastProbe` / `enableActiveNoRouteFastProbe` shorten keepalive interval when failures look imminent. State codes drive the transitions (`shouldEnable*FastProbe*ForStatusCode`).
- **Switching**: `switchToBackupRoute` / `switchToMainRoute` / `switchRouteAndReregister` (re-register on the chosen route). Calls on the old route get re-INVITEd via `reinviteActiveCalls`.
- **Connectivity changes**: `connectivityChanged()` → `prepareConnectivityRecovery` → `recoverTransport` → per-call `reinviteOnConnectivityChange`. Retries are bounded (`MAX_CONNECTIVITY_REINVITE_RETRIES = 3` on `SIPCall`).
- **Suppression**: `shouldSuppressOptionsRecoveryAttempt` rate-limits recovery within a moving window to avoid storming the registrar.

If you see `kaTarget`, `kaMainRoute`, `kaBackupRoute`, `mainRouteFastProbeEnabled_`, or `transportRecoveryPending_` references, this is the system you're in.

## Working in this directory

- **PJSIP API calls happen on the SIP thread** (`SIPVoIPLink::sipThread_`). Cross-thread access must use PJSIP-safe constructs (`pj_mutex`, dispatch onto the SIP thread, or schedule on `Manager::scheduler()` and back).
- **PJSIP pools**: `SIPVoIPLink::getPool()` gives a long-lived caching pool; per-call pools are inside `pjsip_inv_session`. Don't allocate strings outside of pools and pass them to PJSIP.
- **PJSIP modules** are registered once in `SIPVoIPLink::SIPVoIPLink()`; `registerEventPackage` adds dynamic event packages at runtime.
- **SDP**: Negotiation completes asynchronously. `SIPCall::onMediaNegotiationComplete()` is the canonical "media is ready" hook — wire new media setup there.
- **Re-INVITE**: Many paths can fire a re-INVITE (hold/offhold, media change, connectivity change). Read `SIPCall::isReinviteRequired` and `isRestartRequired` before adding new triggers.
- **SRTP** key exchange is SDES only (`KeyExchangeProtocol::SDES`). DTLS-SRTP is **not** implemented.

## Threading & concurrency

- `sipThread_` runs `pjsip_endpt_handle_events` forever; do not block it.
- `Manager::scheduler()` is the off-thread scheduler — use it for slow work, then come back via `runOnMainThread`.
- `SIPCall` has multiple mutexes: `callMutex_` (base), `transportMtx_`, `mediaLifecycleMtx_`, `setupSuccessMutex_`. Most fields are atomics where lock-free is safe.

## Dependencies

- **Internal**: `media/rtp_session.h`, `media/socket_pair.h`, `media/media_codec.h`, `media/media_attribute.h`, `connectivity/{sip_utils,ip_utils}`, `im/message_engine`.
- **External**: pjproject (`pjsip`, `pjsip-ua`, `pjlib`, `pjlib-util`, `pjnath`, `pjmedia`, `pjsip-simple`). Linked from `contrib/<triplet>/lib`.

<!-- MANUAL: -->
