# Implementation Plan — Replace SIP-INFO Conference Metadata with RFC 8285 RTP Header Extensions

**Audience:** the implementer (human or AI agent). **Companion overview (RU):** `docs/rtp-extensions-conference-metadata-overview-ru.md`.
**Repo:** `sip_core` (C++17, patched PJSIP for signaling + FFmpeg/libavformat for media).
**All line numbers were captured on branch `feat/17`; they may drift by a few lines — always grep the named function/symbol to re-anchor before editing.**

---

## 0. Goal & scope

Move the **frame-synchronized** subset of conference metadata off SIP and onto **RTP header extensions (RFC 8285, two-byte form)** so it arrives intrinsically aligned with the RTP timestamp of the frame it describes, eliminating the "new video / old coordinates" artifact.

Three event types (colleague's spec, **amended for MTU efficiency + reorder-safety** — see §3.0 and §3):

| ID | Event | Data | Stream | Cadence |
|----|-------|------|--------|---------|
| `0x01` | A/V-sync `abs-capture-time` | 64-bit NTP capture timestamp | **audio + video, ALWAYS** | per-packet |
| `0x02` | Participant coordinates | index + tile rect (**NO inline URI**) | video | on change, ×3 |
| `0x03` | Voice activity | per-participant bitmask | video | on change, ×3 |

> **SIZE CONSTRAINT: MTU, not a fixed server cap.** The RTP server imposes **no** hard extension-size limit (confirmed with its author). The RFC limits are: per-element data ≤ **255 bytes** (RFC 8285 §4.3, 8-bit length); total extension ≤ **262 140 bytes** (RFC 3550 §5.3.1, 16-bit word count — irrelevant). The **binding** limit is the path MTU (RFC 8085 §3.2): keep the whole IP packet under the effective MTU (IPv6 floor 1280 B, IPv4 576 B) or you hit fragmentation loss/NAT drops. **Design rule (§3.0): target ≤1200-byte packets and reserve a fixed 128-byte extension budget.** `0x02` still carries **only index + coordinates (9 data bytes)** — not for a size cap, but because a small extension preserves media-payload efficiency (the reservation is subtracted from *every* packet) **and** keeps participant URIs off the SRTP-cleartext wire (§3.8). The `index → identity (streamId/URI)` binding rides an out-of-band SIP roster (§7).

**`0x01` is not optional and not layout-related** — it fixes a *separate* audio/video sync problem (audio and video have independent RTP clocks with random offsets; a common capture wall-clock is required to lip-sync without waiting for RTCP SR). It rides **every** audio and video packet. Because audio and video share the same `socket_pair` code path (§4), one implementation covers both.

**Non-goals / stays on SIP (see §7 gap analysis):** `confOrder` (client→host control: mute, raise-hand, hangup, active-stream) stays on SIP (must be reliable). "Decoration" fields (raise-hand, mute icons, moderator, recording, layout mode, canvas dims) stay on SIP `confInfo` in a **hybrid** design.

**sip_core plays BOTH roles** (confirm the primary role per deployment with the RTP-server author, §12): as **conference host** it owns `VideoMixer`, mixes the mosaic, and **sends** `0x01/0x02/0x03` (§5); as a **leaf participant** it **receives** the MCU/host mosaic and applies the extensions to cut tiles (§6). When a dedicated MCU is the mixer, sip_core is a leaf and the send path is exercised only when sip_core itself hosts. The identity model (§7) and the abs-capture-time semantics (§3.5) depend on which role is active.

---

## 1. Current architecture (what you are changing)

### 1.1 Current metadata SEND path (host side)
```
VideoMixer::process()  builds SourceInfo{x,y,w,h,hasVideo,callId,streamId}   src/media/video/video_mixer.cpp (layout: updateLayout() :427)
  -> onSourcesUpdated_ callback (registered Conference::attachVideoMixerCallbacks)   src/conference.cpp:133
      -> ParticipantInfo{...} built, canvas w/h from mixer   src/conference.cpp:171-247
      -> Conference::updateConferenceInfo(newInfo)   src/conference.cpp:257 / body :2029
          -> Conference::sendConferenceInfos()   src/conference.cpp:880   [#if CONFERENCE_METADATA :882]
              -> foreachCall: call->sendConfInfo(getConfInfoHostUri(...).toString())   src/conference.cpp:892-893
                  -> Call::sendConfInfo(json)   src/call.cpp:784  (wraps "application/confInfo+json")
                      -> SIPCall::sendTextMessage(messages, from)   src/sip/sipcall.cpp:2233
                          -> im::sendSipMessage(inviteSession_.get(), messages)   src/sip/sipcall.cpp:2266   [in-dialog only]
                              -> builds request with method = INFO   src/im/instant_messaging.cpp:156-157
Voice activity (throttled, separate): Conference::sendVoiceActivity() :908 -> flushVoiceActivity() :939 -> doSendVoiceActivity() :950
Layout change: Conference::setLayout() :821 -> sendConferenceInfos()
```
Wire JSON: `ConfInfo::toString()` `src/conference.cpp:866` = `{"p":[...],"w","h","v":1,"layout"}`; per-participant `ParticipantInfo::toJson()` `src/conference.h:99-118`. Voice: `voiceActivivtyToString()` `src/conference.cpp:1994` = `[{uri,sinkId,state}]`.

> **Naming caveat (updated — the transport was reverted):** conference metadata travels as an **in-dialog SIP INFO** request body. `im::sendSipMessage` builds the request with `method = INFO` (`src/im/instant_messaging.cpp:156-157`); note the debug log at `instant_messaging.cpp:172` still prints `"sending SIP MESSAGE"` — **misleading; the PJSIP method is INFO.** A brief experiment that delivered this metadata out-of-dialog via SIP MESSAGE was reverted (commits `766f95749`, `49b9e5678`), so we are back to in-dialog INFO. The separate `SIPCall::sendSIPInfo` (`src/sip/sipcall.cpp:1044`) is a *different* INFO sender used only for DTMF, **not** for confInfo. "Replace SIP INFO" is now literally accurate.

### 1.2 Current metadata RECEIVE path (client side) — where the frame is "cut"
```
SIPVoIPLink routes in-dialog INFO body -> Call::onTextMessage   src/sip/sipvoiplink.cpp:1551/1561
  (confInfo now arrives strictly in-dialog; the account-level SIPAccountBase::onTextMessage at src/sip/sipaccountbase.cpp:176 no longer routes conference metadata — the out-of-dialog route was reverted)
Call::onTextMessage   src/call.cpp:355
  "application/confInfo+json"         -> Call::setConferenceInfo(body)          src/call.cpp:628
  "application/confVoiceActivity+json"-> Call::setConferenceVoiceActivity(body) src/call.cpp:689
  "application/confOrder+json"        -> Conference::onConfOrder(...)           src/conference.cpp:1369  (HOST only; KEEP on SIP)
Call::setConferenceInfo  parses -> confInfo_ = newInfo (:671) -> SIPCall::createSinks(confInfo_) (:675) -> emit OnConferenceInfosUpdated (:679)
  -> Manager::createSinkClients   src/manager.cpp:3634
      for each ParticipantInfo (keyed by sinkId): currentSink->setCrop(x,y,w,h)   src/manager.cpp:3664 (existing sink) / :3670 (new sink)
          -> SinkClient::setCrop stores crop_.{x,y,w,h}   src/media/video/sinkclient.cpp:491-498
          -> crop applied on decoder thread in SinkClient::update/applyTransform   src/media/video/sinkclient.cpp:404-410
```
**The artifact's exact site:** `SinkClient::setCrop` (`sinkclient.cpp:491`) writes 4 plain ints from the control thread; `applyTransform` reads them on the decoder thread. New RTP transport delivers *correct coordinates earlier* but does **not** by itself make the apply frame-atomic — see §6.

### 1.3 Media/RTP pipeline (the seam you inject into)
```
SEND:  MediaEncoder -> libavformat RTP muxer (openOutput(dest,"rtp")) -> ff_rtp_send_data builds 12-byte RTP header (X=0)
       -> avio_flush -> SocketPair::writeCallback(buf,size)   src/media/socket_pair.cpp:966   [buf = plaintext full RTP packet]
           -> ff_srtp_encrypt (in place, SRTP)   src/media/socket_pair.cpp:977-989
           -> writeData -> ::sendto   src/media/socket_pair.cpp:894/1002
RECV:  ::recvfrom -> SocketPair::readCallback(buf,size)   src/media/socket_pair.cpp:794
           -> parse_RTP_ext(buf,&abs)   src/media/socket_pair.cpp:846   [existing 0xBEDE one-byte parser :1087]
           -> ff_srtp_decrypt (in place)   src/media/socket_pair.cpp:856
           -> up to libavformat rtpdec -> DISCARDS extension   contrib/.../ffmpeg/libavformat/rtpdec.c:728-741
           -> MediaDecoder -> VideoReceiveThread::publishFrame   src/media/video/video_receive_thread.cpp:96-108
Second recv path (must mirror): SocketPair::readDataNoBlock   src/media/socket_pair.cpp:922-962 (parse_RTP_ext :940, decrypt :950)
Audio uses the SAME SocketPair path: audio_sender.cpp:70-82 (send), audio_rtp_session.cpp:400 (recv), :88/:90-95 (own SocketPair+SRTP)
```

---

## 2. Architecture decision: inject in `socket_pair.cpp`, DO NOT patch FFmpeg

**Verified by two independent research agents + an adversarial verifier.** Summary of evidence:

- FFmpeg's RTP muxer hard-zeroes the X bit (`ff_rtp_send_data`, `rtpenc.c:310`: `avio_w8(pb, RTP_VERSION << 6)` = `0x80`, no `| 0x10`) and its only AVOptions are `payload_type/ssrc/cname/seq/rtpflags` — **no API to add a header extension.** `ff_rtp_send_data` never sees the `AVPacket`, so per-frame metadata can't reach it cleanly.
- FFmpeg's demuxer **discards** extensions (`rtpdec.c:728-741` computes `ext=(AV_RB16(buf+2)+1)<<2` then `buf+=ext`), so receiving through FFmpeg is impossible without a second patch.
- `socket_pair.cpp` is the **only** place a complete, contiguous, plaintext RTP packet is visible — on send (before SRTP encrypt) and receive (after SRTP decrypt) — for **both** audio and video, **codec-agnostic**, with a **working precedent** already present (`parse_RTP_ext`, `socket_pair.cpp:1087`, parses an RFC 8285 one-byte `0xBEDE` extension today).
- **SRTP requires zero changes.** `ff_srtp_encrypt`/`ff_srtp_decrypt` are already extension-aware: they read the X bit and length-in-words generically (they never inspect the profile magic, so `0x1000` two-byte works identically to `0xBEDE`), skip the extension for *encryption*, and cover it with the auth HMAC (`src/media/srtp.c:305-324, 336-337` encrypt; `243-251` decrypt). RFC 3711 leaves header extensions in cleartext-but-authenticated. Note (m1): on **receive** we parse *before* `ff_srtp_decrypt`, so tamper is only detected *after* parsing — for SRTP streams, **commit** decoded geometry only once `ff_srtp_decrypt` returns success (`err ≥ 0`); the timing read may stay pre-decrypt.
- **Precedent-claim correction (M8):** the shipped library hard-zeroes X, **but** an orphan patch `contrib/src/ffmpeg/rtp_ext_abs_send_time.patch` already demonstrates setting `X|0x10` + injecting `0xBEDE` in `ff_rtp_send_data` — it is simply **not applied** (absent from `rules.mak:266-269` / `package.json` patches) and emits the wrong thing (one-byte, 24-bit abs-*send*-time). So the socket_pair approach is self-consistent today; just don't be surprised by the ready-made-but-dormant patch, and delete/reconcile it to avoid future confusion.

> **BLOCKER B2 — abs-send-time coexistence.** The existing `parse_RTP_ext` (`socket_pair.cpp:1087`) reads a one-byte `0xBEDE` **abs-send-time** extension and feeds delay-gradient congestion control (`getOneWayDelayGradient` → REMB, `video_rtp_session.cpp:~628`). RFC 3550 §5.3.1 permits **only ONE** extension block per packet, and RFC 8285 forbids mixing one-byte + two-byte in one block. So our `0x1000` metadata and a `0xBEDE` abs-send-time **cannot coexist as two blocks**. Two consequences: (1) the decoder must **dispatch on profile** and keep the `0xBEDE` path alive (§3.4) — never blanket-"skip unknown", or REMB congestion control silently dies; (2) if the peer/MCU needs a send-time for CC on the same stream that carries `0x1000`, fold it in as an **element inside** the `0x1000` block (e.g. reuse `0x01` abs-capture-time, which can also drive gradient CC), or formally drop it. **Confirm with the RTP-server author whether it emits `0xBEDE` (§12).**

**Ordering constraints (hard):**
- SEND: build the extended packet and set `buf[0] |= 0x10` **before** `ff_srtp_encrypt` (`socket_pair.cpp:977`).
- RECV: parse the extension **before** `ff_srtp_decrypt` (`socket_pair.cpp:856`), i.e. on the still-"encrypted" buffer (header+ext are cleartext).
- `ff_srtp_encrypt` needs distinct in/out buffers (it `memcpy(out,in,len)`); `writeCallback`'s `buf` is `const`. Use a **new** scratch member (e.g. `extBuf_[RTP_MAX_PACKET_LENGTH]`), separate from `encryptbuf` (`socket_pair.cpp:120`). `RTP_MAX_PACKET_LENGTH = 2048` (`socket_pair.cpp:82`) — ample over a ~1500 MTU.
- CC is 0 in our muxer output (`buf[0]=0x80`), so the extension goes immediately after the 12-byte fixed header — no CSRC arithmetic. (Still guard defensively: `cc = buf[0] & 0x0F`, offset = `12 + 4*cc`.)

> If, and only if, the RTP-server author insists FFmpeg own packetization, the fallback is patching `ff_rtp_send_data` (`rtpenc.c:303-325`) + `rtp_write_header` headroom (`rtpenc.c:96-110`) + a `RTPMuxContext` data channel + a `rtpdec.c` receive patch, added to **both** `contrib/src/ffmpeg/rules.mak:266-269` and `contrib/src/ffmpeg/package.json` "patches" (and delete the orphan/redundant `rtp_any_payload.patch`). This is strictly more code, worse thread-safety, and still needs a receive patch. **Not recommended.**

---

## 3. Wire format (RFC 8285 two-byte header) — canonical spec

> This section is authoritative for the encoder/decoder. `abs-capture-time` (`0x01`) is a **WebRTC experiment, not an IETF RFC** — verify its exact byte layout against the RTP-server author's implementation before finalizing.

### 3.0 Size budget — MTU-derived, no fixed server cap

The RTP server imposes **no** hard extension-size limit. The governing limits (verified against the RFCs):
- **Per-element data ≤ 255 bytes** — RFC 8285 §4.3, 8-bit length. (Inline URI would need ≤246; we don't inline it.)
- **Total extension ≤ 262 140 bytes** — RFC 3550 §5.3.1, 16-bit length in 32-bit words. Not the binding limit.
- **Binding limit = path MTU** — RFC 8085 §3.2: never let the whole IP packet exceed the path MTU (fragmentation ⇒ loss + NAT/firewall drops). If PMTU is unknown, stay under the effective MTU floor (EMTU_S): **IPv6 1280 B, IPv4 576 B**. Note 576 is IPv4's *guaranteed-reassembly* floor, not a per-hop limit on modern links — the ≤1200 target below assumes Ethernet-class links (MTU ≥1500) or PMTUD, and would need to drop to a 576-safe budget only on paths known to be constrained.

**Design rule:** the target whole-packet size is **already fixed in the codebase at 1200 bytes** — `sipcall.cpp:238` (`new_mtu = 1200; rtpSession->setMtu(1200)`) → `mtu_` (`rtp_session.h:114`) → `SocketPair::createIOContext(mtu)` (`socket_pair.cpp:666`). There is no PMTU discovery in-tree, so 1200 is the operative value; drive the reservation off that single `mtu_`, do **not** introduce a second hardcoded 1200. Out of it, **reserve a fixed 128-byte extension budget**. The reservation is *static* — ffmpeg packetizes the payload without knowing our post-mux extension, so whatever we reserve in `createIOContext` (§5.4) is subtracted from **every** packet's media payload. Hence the goal is the *smallest* budget that fits the data, not the largest that fits the MTU. 128 B costs ~10% of one packet's payload (and the every-packet reality is just `0x01`'s ~16 B; `0x02`/`0x03` are on-change). Cap is tunable **within `[16, 256]` and MUST be a multiple of 4** — raise to 256 for fewer fragments on very large layouts (~22% reservation). **Do not exceed 256 B**: past ~273 B a single `0x02` element's data would break the RFC 8285 255-byte per-element length (see the clamp below).

Budget the **padded element block** at `cap − 4 (header)` (default `128 − 4 = 124 bytes`). Requiring `cap` to be a multiple of 4 makes `budget_block` a multiple of 4, so `unpadded_block ≤ budget_block ⟺ padded_block ≤ budget_block` — the closed-form formula below is then exact and needs no padding correction.

Element sizes (two-byte form, `[id][len] = 2 bytes` overhead each). `0x02` is a **single** element carrying a fragment of the layout snapshot (one element, N tiles inside — never repeated `id=2` elements, see §3.5):

| Event | Bytes | Notes |
|---|---|---|
| `0x01` abs-capture-time (8-byte NTP) | **10** | per packet; 16-byte offset variant = 18 (avoid) |
| `0x02` snapshot fragment, K tiles | **2 + 5 + 9K** | 5-byte snap header `[gen:2][fragIdx:1][fragCount:1][count:1]` + K×(idx+rect 9B) |
| `0x03` voice activity | **2 + 2 + ⌈(maxIdx+1)/8⌉** | `[vaSeq:2][bitmask]`. **idx-range reconciliation:** `0x02` carries an 8-bit `idx` (0–255), but the mask grows with `maxIdx`. Default: cap `idx ≤ 63` for **both** `0x02` and `0x03` ⇒ mask ≤ 8B ⇒ element ≤ 12B. For >64 tiles, size the mask from the actual `maxIdx` and feed that back into `size(0x03)` at runtime (never assume 12B). |

**Runtime capacity (derive it, do NOT hardcode a participant count — M6/m2):** with `budget_block = cap − 4` (default `128 − 4 = 124`, `cap` a multiple of 4 so the formula is exact),
```
tiles_per_fragment = min(
    floor( (budget_block − size(0x01) − size(0x03_if_present) − 7) / 9 ),   // MTU budget
    27 )                                                                     // RFC 8285 §4.3: 0x02 data = 5+9K ≤ 255 ⇒ K ≤ 27
```
where `7 = 2 (0x02 id+len) + 5 (snap header)`. The **`min(…, 27)` clamp is mandatory** — without it a large tunable cap (e.g. 512 ⇒ formula says 54) would emit a `0x02` element whose data (`5+9K`) exceeds the 1-byte length field ⇒ silent corruption. With the default 128-byte cap: `0x01`(10) only → **11** tiles/fragment (10 + 7 + 11×9=99 = 116 → +4 = 120 ≤ 128 ✓; 12 tiles = 132 > 128); `0x01`(10)+`0x03`(≤12) → **10** tiles; `0x01`(18-byte variant) → **11 / 9** (only / with `0x03`). So a typical ≤11-tile conference sends its whole layout in **one** fragment. The formula must be evaluated at runtime from the actual cap and `0x03` size; keep `assert(4 + padded_block ≤ cap)` (§3.7) as a backstop, not the primary guard.

**Pagination + convergence (B1/M1):** when the conference has more tiles than one fragment holds, split the snapshot across `fragCount` fragments (same `gen`, `fragIdx = 0..fragCount-1`). The receiver buffers fragments per `gen` and **applies a `gen` only when all `fragCount` fragments of that `gen` have arrived**; a strictly-newer `gen` supersedes any incomplete older `gen`. Each on-change snapshot is re-sent ×3 (RFC 8285 §4.1.1) so a single lost fragment is usually recovered by a duplicate; if a whole `gen`'s fragment set never completes, the next layout change bumps `gen` and re-sends in full (self-heal on next change). For the common **fragCount==1** case (≤11 tiles at the default cap) this reduces to "any one surviving copy fully re-syncs."

**Implication:** `0x02` is index+coords only (identity out-of-band, §7) to keep the *static* per-packet reservation small — not because of a hard cap. MTU: reserve a fixed **`cap` bytes (default 128)** on video, ≤18 on audio (§5.4).

### 3.1 RTP fixed header
Set the **X (extension) bit**: `buf[0] |= 0x10` (RFC 3550 §5.1). The extension follows the **CSRC list** (RFC 3550 §5.3.1). Do **not** hardcode offset 12: a conference **mixer may set CC>0**, so compute `cc = buf[0] & 0x0F` and place/parse the extension at offset `12 + 4*cc`. (Our own outbound muxer emits CC=0, but inbound mixed streams from an MCU may carry CSRCs.)

### 3.2 Extension block (RFC 3550 §5.3.1 outer frame + RFC 8285 §4.3 two-byte elements)
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       0x10        | appbits=0 |         length (words)        |   <- 4-byte ext header
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      ID       |    length     |            data ...           |   <- element (two-byte)
```
- `profile` (2 bytes) = **`0x1000`** (= `0x100` top-12-bits + appbits `0`). RFC 8285 §4.3.
- `length` (2 bytes) = number of **32-bit words** of extension **data**, excluding the 4-byte header. RFC 3550 §5.3.1.
- Each element: `[ID:1][len:1][data: len bytes]`. `len` = **byte** count of `data`, excluding the 2 header bytes (no `+1` in two-byte form). RFC 8285 §4.3.
- `ID = 0` = padding byte (no length follows). Zero-pad the **whole element block** to a 32-bit boundary; pad bytes = `0x00`. RFC 8285 §4.1.

**Two unit traps (top implementer bugs):** the *outer* length is **words**; the *per-element* len is **bytes**. And the element block **must** be padded to a 4-byte multiple before you can compute the word count.

### 3.3 Encoder algorithm (per outgoing RTP packet carrying metadata)
1. `cc = buf[0] & 0x0F; base = 12 + 4*cc` (extension goes after the CSRC list, §3.1).
2. Assemble element block: append `0x01` (always, §3.5); append **one** `0x02` snapshot fragment if geometry is being (re)sent; append `0x03` if VA changed. **At most one element per ID** (B3).
3. Zero-pad block to next multiple of 4 bytes with `0x00`.
4. `outer_length_words = padded_block_len / 4`.
5. Prepend 4-byte header: `0x10 0x00` (profile) + `outer_length_words` as 16-bit big-endian.
6. Splice at `base`: preserve `buf[0..base)`, insert the extension, then the original payload `buf[base..buf_size)`; set `buf[0] |= 0x10`; keep seq/ts/ssrc unchanged. (If `buf[0]&0x10` was already set — an existing extension — merge into it instead of overwriting; see §5.2, M5.)
7. Assert `4 + padded_block ≤ cap` (default 128, §3.0); enforce `tiles_per_fragment` by the runtime formula, never by truncation.
8. On-change snapshots (`0x02`/`0x03`): emit on 3 consecutive packets (RFC 8285 §4.1.1).

### 3.4 Decoder algorithm (bounds-checked; handles BOTH profiles — B2/M2/M3)
Inputs: `buf`, received `len`. 
1. If `!(buf[0] & 0x10)` → no extension, done. Else `cc = buf[0] & 0x0F; base = 12 + 4*cc`. **Validate** `len ≥ base + 4` (else drop — malformed/truncated).
2. `profile = AV_RB16(buf+base)`; `ext_words = AV_RB16(buf+base+2)`; `ext_bytes = min(ext_words*4, len − (base+4))` (clamp to the received length — never trust the length word, M3).
3. **Dispatch on profile — do NOT "skip unknown":**
   - `0x1000` → walk the two-byte metadata block (below).
   - `0xBEDE` → the **existing** one-byte abs-send-time path (`parse_RTP_ext` → `getOneWayDelayGradient` → REMB); preserve it (B2).
   - else → ignore (genuinely unknown).
4. Two-byte walk over `data = buf[base+4 .. base+4+ext_bytes)`: at each step, `id = data[i]`; if `id==0` → padding, `i+=1`; else require `i+2+len ≤ ext_bytes` (bounds, M3), read `len=data[i+1]` and `len` data bytes, dispatch on `id`, `i += 2+len`. Ignore unknown IDs (forward-compat). Stop at end of block.

### 3.5 Event payloads
- **`0x01` abs-capture-time** — `data` = 8 bytes: 64-bit NTP fixed-point Q32.32 (high 32 = seconds since 1900 epoch, low 32 = fraction), network byte order. (Optional +8-byte estimated-capture-clock-offset variant → `len=16`; **use `len=8`** to conserve the per-packet reservation — the 16-byte variant lowers `tiles_per_fragment`, §3.0.) Use FFmpeg's `ff_ntp_time()` / `av_gettime()` + NTP epoch offset so the value matches FFmpeg's RTCP SR clock. **Present on every audio and video packet — a per-packet timestamp, NOT an on-change/×3 event.** *(MCU-mixed-mosaic semantics of this timestamp — mix instant vs per-participant capture — is an open question for the server author, §12.)*
- **`0x02` participant snapshot fragment** — a **single** element (never repeated `id=2`, B3). `data` = `[gen:2][fragIdx:1][fragCount:1][count:1]` then `count ×` `[idx:1][x0:2][y0:2][x1:2][y1:2]` (9 bytes each), all uint16 **big-endian**. `gen` = monotonic layout generation (bumped on any layout change); `fragIdx/fragCount` = this fragment's position in the `gen`'s snapshot (§3.0 convergence). `(x0,y0)` top-left, `(x1,y1)` bottom-right of the tile in the combined frame. **NO inline URI** (payload-efficiency + cleartext, §3.0/§3.8) — `idx` binds to identity via the out-of-band roster (§7). **Conversion:** internal model stores top-left `x,y` + `w,h` (`ParticipantInfo`, `conference.h:67-70`) → emit `x0=x, y0=y, x1=x+w, y1=y+h`; receive `w=x1-x0, h=y1-y0`. **Tile presence = membership in the completed snapshot** (an `idx` absent from a complete `gen` ⇒ its sink is destroyed — this is how a leave/mute-off is conveyed, M10).
- **`0x03` voice activity** — `data` = `[vaSeq:2][bitmask]`; bit *i* = participant `idx` *i* talking. `vaSeq` monotonic for reorder rejection. `idx` **capped at 63** ⇒ mask ≤ 8 bytes (m2); if more participants are ever needed, size the mask from the actual max `idx` and account it in the runtime capacity (§3.0).

> **Protocol reconciliation (BLOCKING, §12):** the colleague's published `0x02` uses inline URI and no generation — both must be reconciled **with the RTP-server author** (drop inline URI; adopt this generation/fragment/single-element form, or the server's own equivalent). Do not implement the inline-URI or repeated-`id=2` forms.

### 3.6 Worked examples (all well under the 128-byte reservation)

**(a) Audio packet — only `0x01` (8-byte NTP):**
```
0x01 element: [01][08][8B NTP]        = 10 bytes
pad to 4-byte boundary (+2 x 0x00)    = 12 bytes
outer length (words) = 12/4           = 3  (0x0003)
ext header: 10 00 00 03               = 4 bytes
TOTAL extension                       = 16 bytes   ✓
```

**(b) Video packet — `0x01` + `0x02` snapshot with 4 tiles (single element, fragCount=1):**
```
0x01 element:  [01][08][8B NTP]                              = 10 bytes
0x02 element:  [02][29][gen:2][fragIdx:1][fragCount:1][count=4:1][4 x (idx+8B rect)=36]
               = 2 + (5 + 36) = 2 + 41 = 43 bytes   (len=0x29=41)  -> element 43B
element block                                                = 53 bytes
pad to 4-byte boundary (+3 x 0x00)                           = 56 bytes
outer length (words) = 56/4                                  = 14 (0x000E)
ext header: 10 00 00 0E                                      = 4 bytes
TOTAL extension                                              = 60 bytes  ✓
```
(`0x02` element `len` = data bytes = 5 + 9×count = 5 + 36 = 41 = `0x29`; recompute in code from `5 + 9*count`.) At the default 128-byte cap one fragment holds up to **11** tiles with `0x01` present (§3.0); 4 shown here for brevity.

**(c) Video packet — `0x01` + `0x03`(vaSeq+8B mask) + `0x02` 3 tiles:**
```
0x01 (10) + 0x03 [03][0A][vaSeq:2][8B mask]=12 + 0x02 [02][20][5-hdr + 3x9=27]=34  = 56 bytes
pad (+0, already 4-aligned? 56%4=0)                                               = 56 bytes -> 14 words
TOTAL extension = 4 + 56                                                          = 60 bytes  ✓
```
(`0x03` data = 2 + 8 = 10 ⇒ len=0x0A, element 12B. `0x02` data = 5 + 27 = 32 ⇒ len=0x20, element 34B.) At the 128-byte cap one fragment holds up to **10** tiles when both `0x01` and `0x03` are present (§3.0); 3 shown here.

### 3.7 Encoder/decoder invariants (assert these)
- **Encoder:** `cap % 4 == 0` and `16 ≤ cap ≤ 256`; `4 + padded_block ≤ cap`; `padded_block % 4 == 0`; `outer_length_words == padded_block/4`; each element `len == data_len` in **bytes** (no +1 for two-byte form); at most one element per ID; `0x02.len == 5 + 9*count` **and `0x02.len ≤ 255` (⇒ `count ≤ 27`, RFC 8285 §4.3)**; pad bytes are `0x00`; fragment set for a `gen` covers every live `idx` exactly once.
- **Decoder:** compute `base = 12 + 4*cc`; validate `len ≥ base+4`; clamp `ext_bytes` to the received length; bounds-check every element read; a `0x00` byte = padding (skip 1); dispatch profile `0x1000`/`0xBEDE`/other (never blanket-skip, B2); ignore unknown element IDs; apply a `gen` only when all `fragCount` fragments arrive; reject `gen`/`vaSeq` ≤ last-applied.

### 3.8 Security / privacy (RFC 3711, RFC 8285 §13)
SRTP protects the RTP **payload**, not the header or header extensions — they are integrity-protected but **cleartext**. Any identity data in the extension would be visible on the wire. This is a second, independent reason (besides payload efficiency) to keep participant **URIs out of the extension**. Note: SDES/SRTP setup is currently commented out in this codebase (`src/sip/sdp.cpp:722-723`), so streams may be plain `RTP/AVP` — extension data is in the clear regardless. Extension confidentiality, if ever required, needs RFC 6904 (encrypted header extensions), separately negotiated — out of scope for v1, but record the risk.

---

## 4. SDP negotiation (`a=extmap`)

SDP is **hand-built with PJMEDIA data structures in `src/sip/sdp.cpp`** (not ffmpeg, not the pjmedia negotiator — the negotiator passes unknown `a=` attrs through untouched). There is currently **no** extmap handling anywhere (greenfield).

### 4.1 Emit (offer + answer): `Sdp::addMediaDescription()` — `src/sip/sdp.cpp:427-727`
This single function builds every `m=` and `a=` line and is called for **both** offer (`createOffer` → `sdp.cpp:942`) and answer (`processIncomingOffer` → `sdp.cpp:1012`); the `answering` flag is at `sdp.cpp:437`.

Add a private helper `void Sdp::addRtpExtmapAttributes(pjmedia_sdp_media* med, MediaType type, const pjmedia_sdp_media* remoteMedia)` and call it once, **after** the direction attribute (`sdp.cpp:717-719`) and **before** `return med;` (`sdp.cpp:726`); skip the port-0/disabled early-return branch (`sdp.cpp:638-648`). Gate with `#if CONFERENCE_METADATA` (§8).

Lines to emit:
- Audio m-section: `a=extmap:1 http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time`
- Video m-section: the above **plus** `a=extmap:2 urn:svetets:participant-info` and `a=extmap:4 urn:svetets:voice-activity`

Append idiom (copy the existing fmtp pattern at `sdp.cpp:685-703`, but prefer the **bounds-safe** `pjmedia_sdp_attr_add` used by `addRTCPAttribute` `sdp.cpp:729-736`; the `med->attr[med->attr_count++]=` idiom is unchecked and `PJMEDIA_MAX_SDP_ATTR = 68`):
```cpp
auto v = fmt::format("extmap:{} {}", id, uri);            // name="extmap", value="1 <uri>"
pjmedia_sdp_attr* a = pjmedia_sdp_attr_create(memPool_.get(), v.c_str(), nullptr);
pjmedia_sdp_attr_add(&med->attr_count, med->attr, a);
```

**Offer vs answer (RFC 3264 / RFC 8285 §7):**
- Offer (`answering == false`): emit our own ids (1, 2, 4).
- Answer (`answering == true`, `remoteMedia != nullptr`): **echo the offerer's ids unchanged** for URIs we support; **omit** unsupported ones. Scan `remoteMedia->attr[0..attr_count-1]` for `name=="extmap"` (loop mirrors the rtpmap scan at `sdp.cpp:617-631`), parse `"<id> <uri>"`, re-emit with the **offered** id. Do NOT renumber.

### 4.2 Parse-back & plumb id↔URI into the media layer
`Sdp::getMediaDescriptions(session, remote)` — `src/sip/sdp.cpp:1166-1373` — already iterates `media->attr[j]` (see crypto scan `sdp.cpp:1365-1369`). Add an extmap scan in that loop → populate a **new field** on `MediaDescription`:
```cpp
// src/media/media_codec.h  (struct MediaDescription, ~276-311)
std::map<std::string /*uri*/, uint8_t /*negotiated id*/> rtpHeaderExtensions;
```
`MediaDescription` already flows SDP→media with **no new plumbing**:
```
SIPCall::setupNegotiatedMedia   src/sip/sipcall.cpp:2713
  -> getMediaSlots() -> (localDesc, remoteDesc)   sipcall.cpp:2722
  -> configureRtpSession(...)   sipcall.cpp:2819 (def :224-278)
      -> RtpSession::updateMedia(send, receive){ send_=send; receive_=receive; }   src/media/rtp_session.h:71-73 (fields :112-113)
VideoRtpSession::updateMedia -> new VideoSender(..., send_, *socketPair_, ...)   src/media/video/video_rtp_session.cpp:203-205, ctor :358; VideoSender ctor src/media/video/video_sender.cpp:44-65
```
So `send_.rtpHeaderExtensions["urn:svetets:participant-info"]` etc. is reachable in `VideoSender`/`AudioSender`; forward the id map into `SocketPair` (§5). Read ids from the **local** negotiated session (`activeLocalSession_`, `sdp.h:317-324`); after a compliant answer local==remote ids. Decide authority for non-compliant peers (probably trust the answer/remote id you must write).

### 4.3 RFC conformance notes
- ids 1,2,4 (skipping 3) are legal (unique, non-zero, in two-byte range 1–255). Gaps allowed.
- **Soft SHOULD (RFC 8285 §6):** transmitters intending the two-byte form SHOULD use ids **≥16** to signal that intent (ids ≤14 also fit the one-byte 4-bit field, so a strict peer could mis-assume one-byte). Within our own stack the on-wire `0x1000` magic is authoritative, so 1/2/4 are harmless — but for cross-vendor safety prefer 16/17/18. **The ids are dictated by the RTP-server author; match whatever they define** and note this recommendation to them.
- Reused id `1` on both `m=audio` and `m=video` is fine — sip_core emits separate m-lines with distinct ports (no BUNDLE/`a=mid`, `sdp.cpp:441-446`), so each m-line is its own RTP session / id space.
- Emit **without** a direction token (inherits stream direction) — simplest and correct.
- The stream is **two-byte-only** (never mix one-byte), so `a=extmap-allow-mixed` is **not** required — and MUST NOT be advertised (it would falsely claim mixed-mode receive capability, RFC 8285 §6/§10). Confirm the RTP-server parser accepts low-id two-byte elements.
- Only enable an extension's wire emit if its URI survived in the negotiated (answered) SDP — this is the backward-compat gate (§9).

---

## 5. SEND implementation (inject extension)

### 5.1 Feed metadata down into `SocketPair`
Add setters mirroring `SocketPair::setRtpDelayCallback` (`socket_pair.cpp:1050`) / `setPacketLossCallback` (`:365`):
- `SocketPair::setHeaderExtensionConfig(const std::map<std::string,uint8_t>& uriToId)` — from `MediaDescription.rtpHeaderExtensions`.
- A thread-safe pending-metadata feed, e.g. `SocketPair::setPendingParticipantInfo(gen, bytes)` / `setPendingVoiceActivity(gen, bytes)` (guarded by a small mutex; written by the Conference/sender thread, read in `writeCallback`).
- Capture-NTP source for `0x01` — see §5.3.

### 5.2 Inject in `SocketPair::writeCallback` — `src/media/socket_pair.cpp:966`
At the **top, before `ff_srtp_encrypt` (line 977)**, only for RTP (`!RTP_PT_IS_RTCP(buf[1])`):
1. `cc = buf[0] & 0x0F; base = 12 + 4*cc`. **Defensive X-bit check (M5):** if `buf[0] & 0x10` is *already* set (a pre-existing extension — e.g. if an abs-send-time patch is ever enabled, §2/B2), do **not** blindly treat `buf[base..]` as payload — either merge our elements into the existing block or skip injection for that packet. Our own muxer emits CC=0, X=0, so the common path is `base=12`, X=0.
2. Copy `buf[0..base)` into `extBuf_`; set `extBuf_[0] |= 0x10`.
3. Build the RFC 8285 two-byte extension (§3.3): always `0x01` (from §5.3); add the pending `0x02` snapshot fragment / `0x03` only when (re)sending — recommend the **marker packet** (`buf[1]&0x80`) or the first packet of the access unit; repeat across 3 frames.
4. Append original payload `buf[base..buf_size)` after the extension.
5. Point `buf`/`buf_size` at `extBuf_`; fall through into the existing `ff_srtp_encrypt` (unchanged). **Non-SRTP `RTP/AVP` path (m8):** when SRTP is off, `writeCallback` calls `writeData(buf,…)` directly — this still works because step 5 reassigned `buf=extBuf_`; make sure the extension is applied on **both** the SRTP and non-SRTP branches.

### 5.3 Capture-NTP for `0x01` (the one non-trivial design item)
`writeCallback` sees only the wire RTP timestamp (`buf[4..7]`), a random-offset value — **not** the frame's capture instant, and audio/video clocks differ. Two options:
- **(a)** Establish a per-stream RTP-clock↔NTP reference once (at first frame / from RTCP SR seed) and compute `0x01` in `writeCallback` from `buf[4..7]`.
- **(b)** Plumb capture NTP from the site that owns the `AVFrame`: `VideoSender::encodeAndSendVideo` (`video_sender.cpp:117`) / `AudioSender`, keyed by RTP timestamp, down to `SocketPair`. The RTP timestamp derives from `pkt.pts` in `MediaEncoder` (`media_encoder.cpp:1063-1074`).

Prefer (b) for correctness (true capture time). Use `ff_ntp_time()` / `NTP_TO_RTP_FORMAT` (`rtp.h`) for format consistency. **Do NOT** compute "now" at egress — that reintroduces the jitter this project removes.

**Latch ordering (M7 — get this right or every `0x01` is poisoned):** capture the NTP value **before** `encode()` at the `AVFrame` site, associate it with the frame's access-unit pts, and **persist it across all `writeCallback`s** of that access unit (one encoded video frame fragments into many RTP packets — they must all carry the *same* `0x01`). Store `pts → captureNtp` (last 1–2 access units) reachable from `writeCallback`; look up by the wire RTP timestamp `buf[4..7]`. Single sender thread per stream ⇒ **no mutex needed** (mirror how `setRtpDelayCallback` state is used). A naive "set the NTP after `encode()` returns" races the packets already flushed and corrupts sync — the always-present `0x01` (hard constraint) makes this the one latch you cannot get wrong.

### 5.4 MTU reservation
`SocketPair::createIOContext(const uint16_t mtu)` — `src/media/socket_pair.cpp:666` — sets the ffmpeg payload budget at `socket_pair.cpp:673-674` (`mtu - SRTP_OVERHEAD - UDP_HEADER_SIZE - ip_header_size`), where `mtu` is the RtpSession's `mtu_` (already 1200 today, §3.0). ffmpeg then does `max_payload_size = packet_size - 12` (`rtpenc.c:110`) with **no** knowledge of our post-mux extension. **Subtract the reserved extension size from that same expression** so extended+encrypted packets always stay under `mtu`.
- **Plumbing (m7):** `createIOContext` takes a single `mtu` arg and has no audio/video discriminator. Add an explicit reserve parameter (e.g. `createIOContext(mtu, extReserve)`) or a per-`SocketPair` member set by the video vs audio `RtpSession`, and subtract `extReserve` in the `socket_pair.cpp:673-674` expression. Do not overload `mtu` itself (it must stay the true MTU for other math).
- **Video reserve = the extension cap (default 128 B, §3.0).** **Audio reserve = 0** is acceptable: audio is one frame per packet and never fragments, so audio packets have spare room for the ≤18 B `0x01` without shrinking the payload budget; if you prefer a hard guarantee, reserve `size(0x01)` (16 B for `len=8`; 24 B if the discouraged 16-byte `0x01` is ever used — the ≤18 figure assumes `len=8`).
- This reservation is *static* (subtracted from every packet), which is exactly why the cap is kept small and `0x02` paginates (§3.0) rather than being sized to the full MTU. The reservation plus the encoder's `assert(4+padded_block ≤ cap)` (§3.7) guarantee it is never exceeded; also verify the resulting whole-packet size stays under `mtu` so no IP fragmentation occurs (RFC 8085). The ~10% tax concentrates on the large (keyframe) packets that actually fill the payload budget; small inter-frame packets have headroom to spare.
- **Timing:** this reservation must exist as soon as `0x01` rides every packet — i.e. in the same phase that ships `0x01` (roadmap phase 3), **not** deferred, or phase-3 packets can exceed `mtu`.
- **Considered and rejected — opportunistic injection.** An alternative avoids the static tax: reserve only ~`size(0x01)` and splice `0x02`/`0x03` only onto packets that happen to have MTU headroom (e.g. small end-of-frame packets). Rejected: it makes layout-change delivery best-effort and dependent on packet-size luck, reintroducing exactly the "metadata may lag the frame" race this project exists to kill. The ~10% steady-state payload cost buys deterministic, frame-aligned delivery — the right trade.

### 5.5 Gate the SIP send
When extmap is negotiated for a call, suppress the SIP `confInfo`/`confVoiceActivity` sends for the migrated fields. Seam: `Conference::sendConferenceInfos()` (`conference.cpp:880`) and `doSendVoiceActivity()` (`:950`). **Keep sending** the decoration fields + `confOrder` on SIP (§7). Simplest: keep `sendConferenceInfos()` emitting the hybrid-SIP subset, and route geometry/VA/NTP through the RTP path instead.

---

## 6. RECEIVE implementation (parse + frame-atomic apply)

### 6.1 Parse in `SocketPair::readCallback` — `src/media/socket_pair.cpp:794`
- Extend `SocketPair::parse_RTP_ext` (`socket_pair.cpp:1087-1103`) to **dispatch on profile**, keeping the existing one-byte `0xBEDE` abs-send-time path AND adding the two-byte `0x1000` TLV walk (§3.4, B2) — do **not** replace/remove the `0xBEDE` branch or REMB congestion control regresses.
- **Add a length parameter (M3 — current signature `parse_RTP_ext(uint8_t* buf, float* abs)` has NO size, callers pass only `buf` at `:846`/`:940`).** The outer length word is peer-controlled; on unencrypted streams (§3.8) SRTP's own `if (len < ext) return INVALIDDATA` never runs, so an unbounded TLV walk is an **out-of-bounds read**. New signature must take the received `len`; validate `len ≥ base+4` (`base = 12+4*cc`), clamp `ext_bytes = min(words*4, len − (base+4))`, and bounds-check every element read (`i+2+len ≤ ext_bytes`).
- **Lift the parse OUT of the SRTP-only guard.** Today `parse_RTP_ext` runs only inside `if (... srtpContext_ ...)` (`socket_pair.cpp:839`), so it never fires on an **unencrypted** conference. `0x01` must be read on every packet regardless of SRTP — move the parse above/independent of that branch, in **both** `readCallback` (`:846`) and `readDataNoBlock` (`:940`). Parse before `ff_srtp_decrypt` (`:856`/`:950`); the extension is cleartext. For SRTP streams, **commit** geometry only after decrypt succeeds (m1).
- Read RTP timestamp (`buf[4..7]`) and SSRC (`buf[8..11]`) directly to key the metadata (offset via `base`, not hardcoded 12).

### 6.2 New metadata callback out of `SocketPair`
`SocketPair` has no back-pointer to `Conference`/receive threads (only `packetLossCallback_`/`rtpDelayCallback_` exist, `socket_pair.h:284-285`). Add a `metadataCallback_` + setter (mirror `setRtpDelayCallback` `:1050`), consumed by `VideoReceiveThread`/`AudioReceiveThread`/`Conference`.

### 6.3 Frame-atomic coordinate apply — DO NOT rely on `frame->pts`
**Critical:** `MediaDecoder::decode()` **overwrites** the RTP-derived PTS with local wall-clock before the frame reaches the sink (`src/media/media_decoder.cpp:826` saves it, `:859-865` replaces it via `av_gettime()-startTime_`, `startTime_` at `:683`). So "look up metadata by decoded frame PTS" is impossible — the join key is gone.

**Mandated carrier = AVFrame side-data (M4).** Attach the RTP timestamp + layout `gen` (and the parsed geometry, or a key to it) as **`AVFrame` side-data** in the receive path *before* decode, so it travels **with** the frame through the decoder to `SinkClient`. **Reject** the "ordered per-`SocketPair` queue applied in decode order" alternative: the decoder **drops late frames** (`media_decoder.cpp:842`), so arrival order desyncs from decoded-frame order and the queue would misalign metadata to frames. Side-data is the only carrier that survives frame drops.

Then fix the apply itself: today `SinkClient::setCrop` (`sinkclient.cpp:491-498`) writes 4 ints from the control thread while `applyTransform` reads them on the decoder thread under `mtx_` (`sinkclient.cpp:404-410, 430`) — an unguarded torn-read race (new `x`, stale `w`). Make coordinate application **frame-driven and atomic**: read the `gen`+geometry from the frame's side-data and crop **on the decoder thread**, so generation-N geometry cuts exactly the frame carrying generation-N. Document the RX-thread→decoder-thread handoff (side-data is written on the RX/socket thread, read on the decoder thread — the AVFrame ownership transfer is the synchronization). Otherwise the "old coords cut new frame" artifact reappears one layer down even after the transport change.

### 6.4 Gate the SIP receive
When extmap is negotiated, bypass/ignore the migrated fields in `Call::setConferenceInfo` (`call.cpp:628`) / `setConferenceVoiceActivity` (`call.cpp:689`) so RTP is the source of truth for geometry/VA; keep applying the decoration fields from SIP.

---

## 7. Data model & gap analysis (why it's hybrid)

Current `confInfo` carries ~18 fields/participant. `0x01/0x02/0x03` natively carry only **geometry, identity, voice-activity, capture-NTP**. Mapping:

| Field (src/conference.h) | Consumed at (receive) | RTP-ext home |
|---|---|---|
| `x,y,w,h` (:67-70) | crop `manager.cpp:3664/3670`, `sinkclient.cpp:404` | **0x02** (convert w/h ↔ two corners) |
| `uri` (:63) | sink-key fallback; VA fallback | **SIP roster** (out-of-band, keyed by `idx`) — too big + cleartext for the extension |
| `voiceActivity` (:76) | UI + VA apply `call.cpp:752` | **0x03** (bitmask over `idx`) |
| `sinkId`/streamId (:65) | **PRIMARY crop key** `manager.cpp:3646/3652`; **primary VA match** `call.cpp:737-742` | **SIP roster** `idx → streamId` (RTP `0x02` carries `idx` only) — see identity model below |
| `videoMuted` (:71) | **gates sink create/destroy** `manager.cpp:3651` | **presence** = snapshot membership (an `idx` absent from a complete `gen` ⇒ destroy sink, M10); mute *icon* stays on SIP |
| `active`,`audioLocalMuted`,`audioModeratorMuted`,`isModerator`,`handRaised`,`recording`,`device` | UI / resolution | no home (keep on SIP) |
| canvas `w,h`, `layout`, `v` | UI layout | no home (keep on SIP) |
| abs-capture-time | new | **0x01** (greenfield) |

**Two load-bearing gaps (must address, else rendering breaks):**
1. **`sinkId`/streamId is the real tile key, not URI — and putting either in the extension is wasteful/leaky.** One participant can have two streams (camera + screen-share) → two tiles, same URI. Rather than pay a large static per-packet reservation (and leak identity in cleartext) to carry URI/streamId inline, `0x02` carries a compact **8-bit `idx`** only. **Key the receiver's sink map by `idx`** (not by sinkId) so RTP geometry alone is sufficient to create and crop a tile; the roster (below) supplies display identity. This reworks the sinkId-keyed map (`manager.cpp:3634-3672`) and VA matcher (`call.cpp:737`) to be `idx`-keyed with the roster providing labels.
2. **`videoMuted` = tile presence.** Presence is derived from **snapshot membership**: an `idx` present in the completed `gen` snapshot has a sink; an `idx` absent from it is destroyed (M10). This conveys leave / video-off without a separate field. The mute *icon* (a UI decoration) stays on SIP.

**Identity model + the JOIN race (B4 — the one that can defeat the whole fix).** RTP `0x02`/`0x03` reference participants by `idx`; the `idx → streamId/URI/display-name` binding rides SIP (low cadence). The trap: on **join / layout-grow**, an RTP `0x02` carrying a *new* `idx` can arrive **before** the SIP roster defines it — which would re-introduce the exact SIP-lags-RTP race the project exists to kill, now for tile *presence*. **Fix:** on first sight of an unknown `idx` in a completed snapshot, create a **provisional positional sink keyed by `idx`** and **apply its geometry immediately** (pixels are correct — that's what matters visually); attach a placeholder identity, and **rebind the display identity** (name/URI) when the roster arrives. **Never drop or stall** a frame for an unknown `idx`. State this explicitly: geometry is never blocked on the roster; only the *label* is eventually-consistent. (This makes the residual SIP/RTP race cosmetic — a correctly-placed tile with a momentarily-missing name — not a video artifact.) **Confirm `idx` allocation/lifetime with the RTP-server author** (who owns the mixer).

**Recommendation — HYBRID transport:** RTP carries the frame-synchronized subset (geometry `0x02`, voice-activity `0x03`, abs-capture-time `0x01`), all keyed by `idx`, with geometry self-sufficient for cropping. SIP keeps the low-cadence data: the **`idx → streamId/URI/name` roster**, decoration fields (mute icons, moderator, recording, raise-hand — a 100 ms-late icon causes no artifact), canvas dims, and layout mode. `confOrder` (client→host control) **stays reliable on SIP** unconditionally.

**De-dup / reorder — the generation/fragment fields (wire format §3.5).** The `0x02` snapshot carries `gen` + `fragIdx`/`fragCount`; `0x03` carries `vaSeq`. Receiver: reject any `gen`/`vaSeq` ≤ last-applied (de-dup + reorder rejection); apply a `gen` only when all `fragCount` fragments arrive (§3.0). For **fragCount==1** (the whole snapshot fits one fragment — ≤11 tiles at the default 128-byte cap per the §3.0 formula) any one surviving copy fully re-syncs; for multi-fragment `gen`s, ×3 duplication covers single-fragment loss and a newer `gen` supersedes an incomplete older one (self-heal on next change). `0x01` needs no de-dup (per-packet).

---

## 8. Feature gating

- Compile gate: reuse `CONFERENCE_METADATA` (`CMakeLists.txt:54-56`, exported `:437`). Wrap the extmap emit (§4.1) and RTP inject/parse in `#if CONFERENCE_METADATA`.
- Runtime/negotiated gate: only activate the RTP path (and suppress the corresponding SIP sends) when the peer **echoed** the extmap in its SDP answer. **Note:** the former env-flag helper `confInfoOutOfDialogEnabled()` / `isConferenceControlPayload()` (and env `SIP_CORE_CONFINFO_IN_DIALOG`) were **reverted out** of `conference_protocol.*` (commits `766f95749`/`49b9e5678`) — there is no such pattern to mirror. If a runtime kill-switch is wanted, add a fresh gate at the send seam (`Conference::sendConferenceInfos()` `conference.cpp:880` / `doSendVoiceActivity()` `:950`) driven off the negotiated extmap.
- Consider a distinct build option (e.g. `CONFERENCE_METADATA_RTP`) if you want to ship SIP-only and RTP variants independently.

---

## 9. Backward compatibility & fallback

- If the peer does **not** offer/echo `urn:svetets:participant-info` (etc.), keep the legacy SIP `confInfo`/`confVoiceActivity` path for that call — no behavior change.
- Per RFC 8285 §7, an answerer that doesn't want an extension removes it from the answer; the sender must only emit an extension whose id survived negotiation. This is the interop switch.
- Old ↔ new peers therefore degrade gracefully to SIP.

---

## 10. File-by-file change checklist

| # | File | Function / anchor | Change |
|---|------|-------------------|--------|
| 1 | `src/media/media_codec.h` | `struct MediaDescription` (~276-311) | Add `std::map<std::string,uint8_t> rtpHeaderExtensions;` |
| 2 | `src/sip/sdp.cpp` | `addMediaDescription` (427-727), after dir attr (717-719) | New `addRtpExtmapAttributes()`; emit ids 1(+2,4 video); offer vs answer echo (scan `remoteMedia`, loop like 617-631); `#if CONFERENCE_METADATA` |
| 3 | `src/sip/sdp.cpp` | `getMediaDescriptions` (1166-1373), attr loop (1365-1369) | Parse `a=extmap` → fill `MediaDescription.rtpHeaderExtensions` |
| 4 | `src/media/socket_pair.h` / `.cpp` | new members/setters (mirror 1050/365; scratch buf like 120) | `setHeaderExtensionConfig`, pending-metadata feed, capture-NTP feed, `metadataCallback_`, `extBuf_[2048]` |
| 5 | `src/media/socket_pair.cpp` | `writeCallback` (966), before `ff_srtp_encrypt` (977) | Splice two-byte extension, set `buf[0]|=0x10`, RTP-only |
| 6 | `src/media/socket_pair.cpp` | `createIOContext` (665-684), budget expr (673-674) | Reserve the ext cap (default **128 B**, tunable) on video / ≤18 B audio from MTU budget (m7); keep whole packet ≤1200 B; CC-aware `base=12+4*cc` |
| 7 | `src/media/socket_pair.cpp` | `parse_RTP_ext` (1087-1103) | **Add `len` param + bounds (M3)**; **dispatch BOTH** `0xBEDE` (keep abs-send-time/REMB) and `0x1000` (B2); no blanket-skip |
| 8 | `src/media/socket_pair.cpp` | `readCallback` (794/846) **and** `readDataNoBlock` (922/940) | Parse ext (lift OUT of SRTP guard 839), latch by RTP ts, fire `metadataCallback_`; commit after decrypt success (m1) |
| 9 | `src/media/video/video_sender.cpp` / `audio_sender.cpp` | ctor (44-65 / 70-82), `encodeAndSendVideo` (117) | Pass negotiated ext ids + **capture-NTP latched pre-encode per AU** (M7) into `SocketPair` |
| 10 | `src/media/video/video_receive_thread.cpp` / audio | `publishFrame` (96-108) / decode path | Carry RTP-ts + `gen` + geometry as **AVFrame side-data** (M4 — NOT an ordered queue; decoder drops frames) past the decoder |
| 11 | `src/media/media_decoder.cpp` | `decode` PTS overwrite (826, 859-865) | Preserve RTP ts on a side channel BEFORE overwrite (do not remove the wall-clock pts) |
| 12 | `src/media/video/sinkclient.cpp` | `setCrop` (491-498), `applyTransform` (404-410) | Make crop apply frame-atomic + guarded (fix torn-read race) |
| 13 | `src/conference.cpp` | `sendConferenceInfos` (880), `doSendVoiceActivity` (950) | Route geometry/VA/NTP to RTP; keep hybrid decoration + `confOrder` on SIP when extmap negotiated |
| 14 | `src/conference.cpp` / `src/call.cpp` | build `0x02`/`0x03` bytes; `setConferenceInfo` (628), `setConferenceVoiceActivity` (689) | Encode events by **`idx`** (index-only, +generation), within the ext cap (default 128 B) with pagination; maintain the `idx → streamId` roster on SIP; on receive, resolve `idx`→sink, apply RTP-sourced geometry/VA, ignore SIP duplicates when negotiated |
| 15 | `CMakeLists.txt` | `CONFERENCE_METADATA` (54-56, 437) | (Optional) add `CONFERENCE_METADATA_RTP` option |
| — | `contrib/src/ffmpeg/*` | — | **No change** (do not patch ffmpeg) |
| — | `src/media/srtp.c` | — | **No change** (already extension-aware) |

---

## 11. Validation plan

No committed automated suite (`AGENTS.md §7`). Validate by:
1. `clang-format -i` + `clang-tidy -p out/build/darwin` on every touched file; build Debug **and** Release (`cmake --build out/build/darwin --target sip_core`).
2. Two `sip_cli` instances (or vs the RTP server): confirm SDP shows the three `a=extmap` lines and the answer echoes them.
3. Packet capture (Wireshark / the in-repo `sharkmcp` tooling): verify the RTP X bit set, profile `0x1000`, correct word-length, element TLVs, and that SRTP auth still validates. Confirm `0x01` on **both** audio and video.
4. Layout-change stress: rapidly change layout / add-remove participants; confirm the "new video / old coordinates" artifact is gone and coordinates apply on the correct frame (generation honored, no torn crop). **Join test:** a new `idx` renders (correct pixels) from RTP alone before the SIP roster arrives (B4); its label fills in when the roster lands.
5. A/V sync: verify lip-sync improves and holds without depending on RTCP SR timing.
6. Backward compat: against a peer that doesn't offer extmap, confirm the legacy SIP path still works unchanged. **CC regression (B2):** confirm delay-gradient/REMB still works when the peer emits `0xBEDE` abs-send-time (decoder must not have blanket-skipped it).
7. MTU / budget: assert every emitted extension is ≤ the reserved cap (default 128 B) including the worst-case `0x02` fragment + `0x03` + `0x01`; confirm the whole packet stays ≤1200 B and no IP fragmentation occurs (capture at both ends and on-path).
8. Fuzz the decoder with truncated/oversized `ext_words` and out-of-range element `len` to confirm the bounds checks (M3) prevent OOB reads on unencrypted streams.

---

## 12. Open questions / risks

**A. MUST confirm with the RTP-server author before implementing (protocol-level; cannot be decided unilaterally):**
1. **Wire format of `0x02`** — the published inline-URI form is technically legal now that there is no 64-byte cap (RFC 8285 element ≤255 B ⇒ URI ≤246), but it is rejected here on **payload-efficiency + cleartext-privacy** grounds (§3.0/§3.8): a large *static* per-packet reservation and PII on the wire. Agree on the index-only + generation/fragment single-element form (§3.5), or the server's own equivalent. **Does the server already define a generation/index field we should adopt?**
2. **Does the MCU emit `0xBEDE` abs-send-time (or any other extension) for its own congestion control?** If yes, it cannot coexist as a second block with our `0x1000` (RFC 3550 = one block/packet) — must fold into `0x1000` as an element or drop it (B2, §2/§3.4).
3. **Does the server's RFC 8285 parser accept multiple elements sharing one id, and low ids (≤14) under the two-byte `0x1000` profile?** Both affect correctness (B3 → single-element form; §4.3 → id choice). Decide id values (colleague's 1/2/4 vs RFC-preferred ≥16).
4. **Target MTU / max packet size.** No hard extension cap exists (confirmed); the binding limit is the path MTU (RFC 8085). Confirm the target whole-packet size (this plan uses ≤1200 B) and thus the extension reservation (default 128 B, §3.0/§5.4) — larger if the deployment guarantees a bigger PMTU.
5. **`idx` allocation/lifetime** (who assigns, reuse on leave, range) — drives §7 identity + `0x03` idx cap.
6. **abs-capture-time semantics on a mixed mosaic** — mix instant vs per-participant capture time — and the **audio↔video SSRC/session correlation key** the receiver uses to pair the two streams for lip-sync; how `0x01` interacts with any RTCP SR the server still sends (m9).
7. **sip_core's primary role in your deployment** — host (sends) vs leaf (receives) vs both (§0). Determines which paths are exercised.

**B. Internal design decisions:**
8. **Capture-NTP plumbing (§5.3/M7)** — per-stream RTP↔NTP reference vs plumb from the `AVFrame` site; latch pre-encode per access unit. Decide before coding `0x01`.
9. **Which negotiated id is authoritative** for non-RFC-3264-compliant peers (local vs answered id, §4.2).
10. **URI/geometry confidentiality (§3.8)** — SRTP leaves header extensions cleartext; RFC 6904 if it matters — likely defer (mitigated by keeping URIs off the media plane).
11. **`confOrder` stays on SIP** — confirm no reliability-sensitive control verb accidentally gets moved to RTP.

---

### Appendix: canonical file:line index (re-anchor by symbol before editing)
- Send (SIP, in-dialog INFO): `conference.cpp:880/908/939/950/821/1994/1961`, `call.cpp:784/769/799`, `sipcall.cpp:2233/2266/1044`, `im/instant_messaging.cpp:147/156-157`, `conference.h:99/80/120`.
- Receive (SIP): `sipvoiplink.cpp:1551/1561`, `sipaccountbase.cpp:176`, `call.cpp:355/628/689`, `conference.cpp:1369`.
- The cut: `manager.cpp:3634/3664/3670`, `sipcall.cpp:4026/4078`, `sinkclient.cpp:491/404`.
- Media seam: `socket_pair.cpp:966/977/794/846/856/922/940/950/1087/665-684/673-674/1050/82/120`, `srtp.c:305-324/336-337/243-251`.
- FFmpeg (no change): `rtpenc.c:303-325/110/310`, `rtpdec.c:679-741/695/757`.
- Decoder PTS overwrite: `media_decoder.cpp:826/859-865/683`.
- SDP: `sdp.cpp:427-727/437/942/1012/617-631/685-703/729-736/1166-1373/1365-1369/1160-1163/1375-1386/119-127`, `sdp.h:317-324`.
- Flow: `media_codec.h:276-311`, `rtp_session.h:71-73/112-113`, `sipcall.cpp:2713/224-278`, `video_rtp_session.cpp:203-205/358`, `video_sender.cpp:44-65/117`, `audio_sender.cpp:70-82`, `audio_rtp_session.cpp:88/400`.
- Signal: `callmanager_interface.h:360`. Gate: `CMakeLists.txt:54-56/437` (no runtime env-flag helper exists post-revert — see §8).
