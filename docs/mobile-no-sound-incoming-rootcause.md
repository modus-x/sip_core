# No sound on calls — Android v0.13.8 (revision 5)

> **Read revision 5 first (§15–§17, end of file).** It carries the field verdict: the cause was an
> RTP header extension the PSTN gateway mishandles, not the media rebuild revision 4 blamed.
> §9–§14 (revision 4) remain accurate as measurements and describe a real, separate defect that was
> also fixed. §0–§8 are revision 3, kept for the case 1–3 analysis.

**Revision 3.** Scope narrowed to Android. iOS is closed: the latest TestFlight builds are reported
100 % good and the iOS logs have been removed from `logs/`. Revision 2's iOS sections are dropped
wholesale, not carried forward.

Evidence: `logs/android_no_sound_incoming/{1,2,3}/` — three PCAPdroid captures plus three Android
Studio logcats. Device Xiaomi MI 6 / Android 9 / MIUI / Qualcomm msm8998, app
`ru.rostelecom.vatsmobilelk` v1.3.8(252) from the market, user `kate`, domain `TDEIP.21.rt.ru`.
Library analysed on branch `research/android-nosound-v0.13.8` @ `00251ef8` (`VERSION 0.13.8`).
`git merge-base --is-ancestor 76d33574e 00251ef8` → **false**: the `AudioDeviceGuard` linger rewrite
is absent from this tree.

---

## 0. The headline, before any mechanism

**The three field cases are three different problems. One is proven server-side, one is a proven
client failure, and one cannot be decided from the evidence captured.**

| case | direction | peer | what the wire shows | verdict |
|---|---|---|---|---|
| 1 | incoming | `79963540961` → kate | inbound real speech, peak −5.8 dBFS, RTCP RR confirms 0 lost; track landed on **output 13** (mode race lost by 12 ms) | **UNDETERMINED.** The client received it. Whether it rendered it or rendered silence cannot be told from this evidence — see §1 |
| 2 call A | outgoing | kate → `89963540961` | 1690 packets, seq 46135..47824 gapless, **270210 of 270400 payload bytes are `0xD5`**, peak 24/32768 (−62 dBFS) | **server-side digital silence** |
| 2 call B | outgoing | kate → `89963540961` | 1147 packets gapless, **all 183 520 payload bytes are `0xD5`**, peak 8/32768 | **server-side digital silence** |
| 2 call C | outgoing | kate → `89372250814` | real audio, clean 425 Hz Russian ringback at −16.5 dBov, max RMS −15.8 dBov | **worked** |
| 3 call 11 | outgoing | kate → `89963540961` | pre-answer 88.6 % silence (normal ringback duty cycle), **post-answer 62.7 % non-silence over 556 packets** | **real audio arrived and the user heard nothing — THE client bug** |

The peer number is the strongest correlate in the whole dataset. `+7 996 354 0961` and
`8 996 354 0961` are the same subscriber. Every failing call in cases 1–3 involves that number; the
one call to a different number (`89372250814`) worked and carried a textbook 425 Hz ringback.

**Consequence for revision 3's mandate.** The brief for this revision stated as ground truth that in
all cases other than case 2's two silent streams "the client received all audio but did not play it".
That is **confirmed for case 3**, **refuted for case 2 calls A and B** (they received nothing to
play — the server sent digital silence), and **undetermined for case 1**. Case 1 remains a live
candidate for a client playback failure; §1 explains why the evidence that seemed to clear it does
not.

**And the one genuine client failure has no client-side logs.** See §4.

---

## 1. Case 1 — undetermined. The evidence that seemed to clear it does not.

Call-ID `c6gu-fDCwC1nxZkTW@10.243.195.7`, sip_core call `8493152281988565`, INVITE from
`"79963540961" <sip:79963540961@TDEIP.21.rt.ru>` (UA `RTC CallManager 9.2-300`). PCMA/8000,
ptime 20 ms, sendrecv, no ICE, no SRTP.

**Inbound carried loud continuous speech.** 420 packets × 160 B, seq 39723→40142 contiguous, 0 lost,
mean delta 19.812 ms, max jitter 4.5 ms. Server SR self-reports `packetcount 420, octetcount 67200`,
matching the capture exactly. Decoded A-law: 46.7 % of packets classify as speech (RMS ≥ 300/32768),
peak −5.8 dBFS at t+1.00 s, sustained speech from t+0.75 s to t+9.00 s.

**The client's own RTCP Receiver Report proves the receiver consumed it.** At 1788268753.971132 the
client sent an RR for SSRC `0xdcb2d388` with `Fraction lost: 0/256`, `Cumulative lost: 0`,
`Extended highest sequence number received: 40046`, `Interarrival jitter: 3`.

**And Android counted a full call's worth of frames.** At hangup:

    1788268755.674  AudioTrack  stop(sessionID=2561)
    1788268755.674  AudioTrack  stop() called with 409728 frames delivered

409 728 frames @ 48 kHz = 8.536 s, against 8.400 s of inbound media (420 × 160 samples @ 8 kHz).

### 1a. That number proves nothing, and this is the single most important methodological point in the report

The upper layer genuinely cannot fabricate silence. `RingBuffer::get()` returns `{}` when empty
(`ringbuffer.cpp:240-243`); `RingBufferPool::getData()` returns `nullptr` when nothing mixed
(`ringbufferpool.cpp:308`); `getToPlay()` feeds silence **only** to the AEC reference and then
`break`s with a null frame (`audiolayer.cpp:346-356`); and `engineServicePlay()` enqueues nothing at
all into `playBufQueue_` when `getToPlay()` fails (`opensllayer.cpp:269-295`).

**But the OpenSL buffer-queue callback one level below re-primes the device with silence**
(`src/media/audio/opensl/audio_player.cpp:87-96`):

    while (playQueue_->front(&buf) && devShadowQueue_.push(buf)) { ... Enqueue ... }
    if (devShadowQueue_.size() == 0) {
        for (int i = 0; i < DEVICE_SHADOW_BUFFER_QUEUE_LEN; i++) {
            if ((*bq)->Enqueue(bq, silentBuf_.buf_, silentBuf_.size_) == SL_RESULT_SUCCESS) {
                devShadowQueue_.push(&silentBuf_);

`playQueue_` is `playBufQueue_` (`opensllayer.cpp:71`), and `silentBuf_` is `memset` to zero once at
`audio_player.cpp:169-171`. So when the upper layer starves, the `while` does nothing,
`devShadowQueue_` drains to zero, and the device queue is re-primed with four zero-filled buffers.
**The device queue is never allowed to run dry, and AudioTrack keeps counting frames.**

Therefore a completely starved pipeline still produces continuous frame delivery.
**`409728 frames delivered` cannot distinguish "played the operator" from "rendered 8.5 s of
silence", and must not be used as evidence in either direction.**

This retracts a conclusion stated earlier in this investigation, and it also retracts revision 2's:
revision 2 refuted the Android root cause partly on "409728 frames delivered onto a settled, unmuted
earpiece". That argument was never valid. Revision 1's Android claim was dismissed on bad grounds —
which does not make it right, but it does mean it was never actually tested.

**What case 1 still supports.** Inbound real speech: yes, proven at the wire. Receiver consumed it:
yes, proven by the client's own RTCP RR. Rendered to the ear: **unknown**. And case 1 is the call
that **lost** the mode race by 12 ms (§5a), so its track ran the whole call on output 13 with the
QCOM VoIP topology absent.

### 1b. Reading the case-1 complaint

 "звонили с сотового на вх.линию / не слышно вызываемого
абонента (не слышно оператора)" — the caller is the mobile; the *called* party, the operator, is
kate. The complaint is that **the caller could not hear kate**. Kate's uplink did carry real speech
(peak −3.5 dBFS, 39.6 % speech packets), so that direction's loss is downstream of the phone, on the
same peer leg implicated in case 2. This is independent of §1a: the uplink question is settled, the
downlink (did kate hear the operator) is not.

---

## 2. Case 2 — two calls of byte-level server silence, and a control that worked

Three consecutive outgoing calls in one 113 s window. Capture t0 = 1788269334.458518.

| | Call-ID | callee | INVITE | 183 | 200 OK | BYE | RTP ports |
|---|---|---|---|---|---|---|---|
| A | `Yewoe6gekGP.C5cd2TBf-3m-2Tf9Evi2` | 8996354**0961** | …360.957 | 362.729 | 375.650 | 396.919 | 54956↔25624 |
| B | `I8hE4bjkM.gAILe7INLt4jG75jdKVris` | 8996354**0961** | …405.789 | 407.584 | 416.575 | 430.824 | 59996↔37648 |
| C | `JlGY34WkPyL71Sk0gryFNfFTpROtz6IQ` | 8937225**0814** | …436.312 | 438.084 | 447.085 | 460.930 | 53258↔19666 |

All three negotiated identically: offer `audio <port> RTP/AVP 104 8 0 101`, answer
`audio <port> RTP/AVP 8 101`, sendrecv, PCMA 8000, server media address unchanged between 183 and
200 OK.

**Call A inbound** `212.122.2.180:25624 → 10.215.173.1:54956` SSRC `0xC583B65E`: 1690 packets,
seq 46135..47824 with zero gaps, 33.78 s. 270 210 of 270 400 payload bytes are `0xD5` (A-law +0);
107 × `0x54`, 83 × `0x55`. Peak decoded amplitude 24/32768 (−62 dBFS). 100 % of packets below
−50 dBov, max RMS −66.2 dBov. The only two packets containing anything else arrive at …397.031 and
…397.052 — *after* the client's BYE.

**Call B inbound** SSRC `0xEE3D3463`: 1147 packets, seq 48567..49713 with zero gaps, 22.92 s.
**All 183 520 payload bytes are `0xD5`.** Peak amplitude 8/32768. Median = p95 = max RMS = −72.2 dBov.

**Call C inbound** SSRC `0xA831EA68`: 1125 packets, 22.3 % of packets above −35 dBov, max RMS
−15.8 dBov, peak amplitude 19968, and a clean **425 Hz ringback** at −16.5 dBov from …444.244.

**The uplink was healthy in all three**, statistically indistinguishable between dead and good calls:

| uplink SSRC | call | pkts | > −35 dBov | p95 RMS | max RMS | peak |
|---|---|---|---|---|---|---|
| `0x24B8C873` | A | 1055 | 28.1 % | −18.7 | −9.1 | 31232 |
| `0x7CFB5A4E` | B | 677 | 30.4 % | −15.6 | −5.8 | 32256 |
| `0xE3F13F60` | C | 685 | 27.2 % | −15.3 | −11.5 | 31232 |

So "АКУСТИКА ОТСУТСТВУЕТ с обеих сторон" is half-refuted at the wire: the phone captured, encoded
and sent the user's voice normally in both dead calls. A gapless, perfectly-sequenced, byte-zero
A-law stream is a media server emitting silence, not a network or client fault.

---

## 3. Case 3 — the one real client-side failure

Reconstructed independently by sharkd (`voip_calls`), and the direction is proven by flow, not timing.
Call 11, Call-ID `rTthKnek850vYNQWP1qRSMoUK.DhbWTd`, kate → `89963540961`, **outgoing**:

    456.166053  [0->1]  INVITE
    456.383480  [1->0]  401 Unauthorized
    456.384657  [0->1]  INVITE (authenticated)
    458.034577  [1->0]  183 Session Progress
    466.553232  [1->0]  200 OK
    466.555448  [0->1]  ACK
    477.614794  [0->1]  BYE            <- phone hangs up
    477.813631  [1->0]  200 OK (BYE)

INVITE→BYE = 21.449 s, matching the report "02.09.26 10:42 21 сек. я не слышала оператора".

Inbound stream SSRC `0x9E8FE688`, split at the 200 OK (epoch 1788334970.672):

    PRE-ANSWER   401 packets   64160 bytes   silence 88.6%   non-silence 11.4%
    POST-ANSWER  556 packets   88960 bytes   silence 37.3%   non-silence 62.7%

The pre-answer profile is a normal Russian ringback duty cycle. **The post-answer profile is
substantial, sustained real audio — 62.7 % non-silence across 11 seconds — and the user heard
nothing.** This is the only call in the dataset where real speech demonstrably arrived and was
demonstrably not heard.

### 3a. …and it has no client-side logs at all

    last SIP packet anywhere in case-3 pcap : epoch 1788335020.072  (BYE of call 12)
    first logcat record                     : epoch 1788335110.096
    gap                                     : 90.02 s

Verified against the raw logcat JSON, not the TSV: 12 349 records spanning
1788335110.096 … 1788335297.784. Every one of the 92 `sip_core` lines is either a 15.001 s-period
keep-alive (`sipaccount.cpp:196`) or app teardown from 1788335220.359 — which includes, verbatim,
`manager.cpp:774 ]Hangup 0 remaining call(s)`. There is no call in that logcat to correlate against.

Two independent capture defects compound here: the logcat was started ~90 s after the last call
ended, **and** it was filtered to `package:ru.rostelecom.vatsmobilelk`, which drops
`audioserver` / `AudioFlinger` / `audio_hw` entirely (0 occurrences of `getOutputForAttr`,
`AudioTrack`, `setPhoneState` — confirmed).

### 3b. Clock calibration — three clocks, and getting them wrong invalidates everything

- Normalized TSV column 2 was rendered with `datetime.fromtimestamp()` on the analysis machine =
  **Moscow, UTC+3**.
- `libsip_core` prints its own timestamp inside the message body in **device local time = UTC+4**.
  Epoch 1788335119.839 renders as `11:45:19.839` inside a `manager.cpp:169` message while the TSV
  column says `10:45:19.839`.
- The reported "10:42" resolves to a call only under the Moscow reading; in device-local time
  nothing exists at 10:42, because the capture starts at device-local 11:35.

---

## 4. What to do next — the investigation is blocked on one capture

The dataset contains exactly one genuine client playback failure (case 3, call 11) and no client
logs for it. No amount of further source reading closes that gap.

**Required capture, on a call to `89963540961` that reproduces the symptom:**

1. Start `adb logcat` **before** placing the call, and leave it running until after hangup.
2. **No `package:` filter.** `audioserver`, `AudioFlinger`, `audio_hw_primary`,
   `APM_AudioPolicyManager` and `AudioTrack` all live in other processes and are the decisive
   evidence.
3. Capture `dumpsys media.audio_flinger` **during** the call, while the audio is dead.
4. Note the wall-clock instant the user first notices silence, and the phone's timezone.
5. Keep the simultaneous PCAPdroid capture.

With those five, the mechanisms in §5 become testable in a single pass.

**Three one-line log statements would settle almost everything**, and they are worth adding to a
debug build before the next capture:

1. In `getToPlay()` (`audiolayer.cpp:327ff`) — log which branch was taken (urgent / tone / call data
   / starved), rate-limited to once per second. This alone decides §5d and §5e-ter, and it is the
   only way to tell §1a's two possibilities apart.
2. In `Manager::peerAnsweredCall` (`manager.cpp:1862`) — log the value of `isCurrentCall(call)`
   before the guarded `stopTone()`.
3. In `AudioPlayer`'s buffer-queue callback (`audio_player.cpp:87`) — count `silentBuf_` re-primes
   and log the total at `stop()`, next to the frame count. That makes every future AudioTrack frame
   figure interpretable instead of ambiguous.

---

## 5. Confirmed defects worth fixing regardless of case 3

These are all independently verified on `00251ef8` or in the field logs. None is yet proven to have
caused a field incident, and each is stated at the confidence it has earned.

### 5a. The MIUI mode race — CONFIRMED IN THE FIELD, measured winning AND losing

**The race was caught flipping outcome between two real calls, on a ~10-15 ms margin.** Timestamps
cross-validated three ways — the native library's self-embedded epoch, the logcat wall clock, and
`frame.time_epoch` from the pcap — self-consistent to under 2 ms.

    CASE 1, incoming — RACE LOST by 12 ms
      746.731  getOutputForAttr() ... session 2561 ... returns output 13   <- routing decided
      746.734  AudioTrack  AUDIO_OUTPUT_FLAG_FAST successful               <- wrong path granted
      746.743  AudioPolicyManagerCustom  setPhoneState() state 3           <- 12 ms too late
      session 2561 is never touched again; it stays on output 13 for the whole ~9 s call

    CASE 2 call B, outgoing — RACE WON by 11 ms
      407.603  AudioPolicyManagerCustom  setPhoneState() state 3           <- lands first
      407.614  getOutputForAttr() ... session 2785                         <- 11 ms later
      407.617  AudioPolicyManagerCustom  Set VoIP and Direct output flags for PCM format
      407.630  AUDIO_OUTPUT_FLAG_FAST denied by server; frameCount 0 -> 5772

Same mechanism, same margin, opposite sides of it. `FAST granted` ↔ output 13 and `FAST denied` ↔
the dedicated VoIP path are the two signatures to grep for in any future capture.

**The dominant fixable contributor is measured, and it is not the OS.** In case 1 the native library
emitted `call.cpp:238 emit client call state change CURRENT` at 746.670. `EngineService`'s
`updateInfoSdkCall()` did not run until 746.741 — a **71 ms** Handler/Looper dispatch hop. Once
invoked, `AudioService.setMode()` → `setPhoneState()` took about **1 ms** (746.742 → 746.743).
Remove the 71 ms hop and `setPhoneState(3)` lands roughly 60 ms *before* `getOutputForAttr()`,
turning case 1's 12 ms loss into a comfortable win.



Case 1, verbatim, shows both the capture and playback devices opening while the HAL is still in
`MODE_NORMAL`:

    746.700  audio_hw_primary  adev_input_allow_hifi_record flags=0, source=7, adev->mode=0
    746.701  audio_hw_primary  adev_update_voice_comm_input_stream compress voip not active, use defaults
    746.717  audio_hw_primary  adev_input_allow_hifi_record flags=0, source=7, adev->mode=0
    746.730  audio_recorder.cpp:135  Actual performance mode is 0
    746.731  audio_player.cpp:105    Creating OpenSL playback stream {s16, 1 channels, 4...
    746.731  AudioTrack        set(): streamType 0, sampleRate 48000, ... flags #104
    746.732  APM_AudioPolicyManager  getOutputForAttr() returns output 13 selectedDeviceId 3
    746.732  AudioFlinger      createTrack_l(): mismatch between requested flags (00000104) and output flags (00000006)
    746.734  AudioTrack        AUDIO_OUTPUT_FLAG_FAST successful; frameCount 0 -> 768
    746.741  Engine audio      audio mode 0                    <- app reads getMode(): still 0
    746.743  AudioPolicyManagerCustom  setPhoneState() state 3
    746.743  audio_hw_primary  adev_set_mode: mode 3
    746.806  Engine audio      change audio mode 3

`Set VoIP and Direct output flags for PCM format` occurs **0 times** in the whole case-1 log. FAST is
granted, which is what makes output 13 (PRIMARY|FAST, the music path) eligible, so the in-call track
loses the QCOM VoIP DSP topology — `voice-handset`, hardware AEC, sidetone. Revision 2 measured only
the playback side; the capture side takes the same hit.

**Ownership is settled.** `sip_core_ffi_glue` never touches `android.media.AudioManager` (proven by
repo-wide grep), and `startAudio()` is `@Dart(Skip) @Java(Skip)` in `lime/configuration-ctl.lime:55`,
i.e. Swift/iOS only — Android has no way to call it. The app's single
`setMode(MODE_IN_COMMUNICATION)` is `rtk` `app/.../Engine/EngineService.java:399`, gated on the async
`"SipCallStateConnected"` event delivered via a JNI callback thread →
`Handler.sendToTarget()` → main Looper, with **no synchronization** against the native thread that
opens the device. The delay is not the ~11 ms revision 2 quoted; it is unbounded and scales with
main-thread congestion, which peaks during call setup (Activity launch, fragment transactions,
ringtone teardown all on that queue). That is a sufficient explanation for intermittency.

Verified identical in the shipped build: `f145366` ("ver 252 for gp release") vs HEAD `33e27ef`
differ only in unrelated ANR and null-safety work; the `setMode` gate is byte-identical.

**Fix (app):** set `MODE_IN_COMMUNICATION` at accept time, before calling into the library, rather
than on the "Connected" round trip.
**Fix (library, complementary):** `audio_player.cpp` sets only `SL_ANDROID_KEY_STREAM_TYPE` and never
`SL_ANDROID_KEY_PERFORMANCE_MODE`, so every player requests FAST|RAW. Requesting
`SL_ANDROID_PERFORMANCE_NONE` for the in-call player makes output 13 ineligible. The codebase already
queries the key in `audio_recorder.cpp:127-135` — the field log's `Actual performance mode is 0`
comes from exactly there.

### 5b. `startStream` failure is swallowed and then latched — REAL, did not fire in these calls

`src/media/audio/opensl/opensllayer.cpp:64-113`. `SLASSERT` throws
(`src/media/audio/opensl/audio_common.h:50-54`), so any OpenSL error in the `AudioPlayer` ctor or
`start()` unwinds into a `catch` that logs one `SIP_CORE_ERR` and leaves `player_` null. Then, at
the end of the same function, unconditionally:

    SIP_CORE_WARN("OpenSL audio layer started");
    status_ = Status::Started;

`isStarted()` / `waitForStart()` therefore report success with **no player object**.
`engineServicePlay` is the only consumer of `RingBufferPool::getData(DEFAULT_ID)`, so with no player
the call's inbound buffer is written by the receive thread and never drained — silence for the whole
call, one error line, no recovery. It cannot recover because `AudioDeviceGuard` (`manager.cpp:2176-2187`)
only calls `startStream` on the 0→1 transition; after the failed attempt the refcount is already 1,
so every later `startAudioStream(PLAYBACK)` is a 1→2 no-op.

**Not the cause of these three cases:** no `Error initializing audio playback` line appears in any of
the three logcats.

**Fix:** set `status_ = Status::Started` only when the stream actually came up, and let a failed
`startStream` release the guard so a retry is possible.

### 5c. Unchecked `hw_infos` read — REAL undefined behaviour, latent

`src/media/audio/opensl/opensllayer.cpp:176-180`, verified verbatim on `00251ef8`:

    std::vector<int32_t> hw_infos;
    hw_infos.reserve(4);                                   // capacity only; size stays 0
    emitSignal<libsip_core::ConfigurationSignal::GetHardwareAudioFormat>(&hw_infos);
    hardwareFormat_ = AudioFormat(hw_infos[0], 1);         // UB when size() == 0
    hardwareBuffSize_ = hw_infos[1];                       // UB when size() < 2

The producer, glue `ClientImpl::getHardwareAudioFormat()` (`src/client.cpp:473-492`), pushes nothing
and signals no error when `configurationNotifiers_` is empty; `ClientImpl::broadcast()`
(`src/client.h:73-85`) likewise silently no-ops on an empty notifier vector, and nothing enforces
`loadSignals()` before `startLibrary()`. If it fires, garbage sample rate and buffer size size the
OpenSL buffers with no crash and no log.

**Did not fire in case 1:** `opensllayer.cpp:175 OpenSL init started` at 744.977 is followed by
`OpenSL init: using buffer of 384 bytes to support {s16, 1 c…` — sane values. Cases 2 and 3 show no
`OpenSL init` line at all; init is lazy (`if (!engineObject_)`) and had already run.

**Fix:** check `hw_infos.size() >= 2` and fall back to defaults.

### 5d. Tone priority outranks call audio — mechanism identified, not yet excluded

`src/media/audio/audiolayer.cpp:327ff`, `getToPlay()`:

    if (auto urgentSamples = urgentRingBuffer_.get(RingBufferPool::DEFAULT_ID)) {
        bufferPool.discard(1, RingBufferPool::DEFAULT_ID);
        ...
    } else if (auto toneToPlay = Manager::instance().getTelephoneTone()) {
        ...
    } else if (auto buf = bufferPool.getData(RingBufferPool::DEFAULT_ID)) {
        ...

The telephone tone is checked **before** the call's ring buffer. For as long as
`Manager::getTelephoneTone()` returns non-null, the player renders the tone and
`getData(DEFAULT_ID)` is never reached: inbound audio accumulates in a buffer nobody drains, for the
whole call, with continuous frame delivery and not one log line. That is precisely the case-3
profile. The urgent branch additionally calls `discard(1, DEFAULT_ID)`, dropping call data whenever
urgent samples win.

The clearing call is `stopTone()` in `Manager::answerCall` (`manager.cpp:956`) — the same call that
drives the playback guard 1→0. One function sits on the critical path of two independent failure
modes.

**Open question that decides it:** Russian ringback is ~1 s tone / 4 s silence, so a stuck ringback
should present as intermittent beeping, which kate would likely have reported rather than "I could
not hear the operator". Whether a post-answer tone renders audibly or as its silent phase determines
whether this mechanism survives. Untestable without the case-3 re-capture.

### 5e-bis. `mediaRestartRequired_` is initialised true and NEVER cleared — every SDP negotiation restarts all media

Verified by exhaustive grep on `00251ef8`. There are exactly three occurrences in the tree:

    src/sip/sipcall.h:398:      bool mediaRestartRequired_ {true};      // initialised true
    src/sip/sipcall.cpp:2213:   this->mediaRestartRequired_ = true;     // the ONLY assignment
    src/sip/sipcall.cpp:2258:   if (this_->mediaRestartRequired_) {     // the only read

Nothing ever sets it false. So the guard at `sipcall.cpp:2258` is permanently true and
`SIPCall::onMediaNegotiationComplete()` **always** takes the restart branch:

    2258   if (this_->mediaRestartRequired_) {
    2259       this_->setupNegotiatedMedia();
    2264       this_->stopAllMedia();
    2265       this_->startAllMedia();

`onMediaNegotiationComplete()` is invoked from PJSIP's `sdp_media_update_cb`
(`src/sip/sipvoiplink.cpp:1189`), which fires on **every** completed SDP negotiation in the dialog.

**Consequence: an outgoing call with early media gets TWO full media teardown/rebuild cycles** — one
at the 183 with SDP, one at the 200 OK. An incoming call without early media gets one, at answer.
The flag looks like a dormant optimisation that was never wired up; as written it is dead, and the
"does not require a restart" path at `sipcall.cpp:2210-2216` can never be reached with it false.

`startAllMedia` (`sipcall.cpp:1857`) → `AudioRtpSession::start()` (`audio_rtp_session.cpp:194-219`)
→ new `SocketPair` at `:204` → `startSender()` at `:217` → `sender_.reset(new AudioSender(...))`
→ `AudioSender::setup` (`audio_sender.cpp:60-86`) → `openOutput(dest_, "rtp")`, which does
`avformat_free_context` + `avformat_alloc_output_context2`. `grep -rn '"ssrc"' src/` returns
**nothing**, so sip_core never pins libavformat's SSRC AVOption and each fresh muxer picks a random
one; only the sequence number is carried across via `setInitSeqVal` (`audio_sender.cpp:75`).

**That is the fingerprint in §5f**: a new SSRC on a continuous sequence number, on the same 5-tuple,
is `AudioRtpSession::startSender()` running again — nothing else in the tree produces it. Measured
ring durations between the two negotiations confirm the boundary is 183→200 OK and not a re-INVITE:

    case 2:  28.270 -> 41.191 (12.44 s)   73.125 -> 82.116 (8.61 s)   103.626 -> 112.626 (8.57 s)
    case 3: 405.564 -> 415.303 ( 9.74 s)  458.035 -> 466.553 (8.52 s)  496.128 -> 504.750 (8.62 s)

**Why this matters for case 3.** `stopAllMedia()`/`startAllMedia()` rebuilds the receiver as well as
the sender. Case 3's failing call is outgoing, so it takes this cycle twice, and the second one lands
at the 200 OK — exactly where the user stops hearing the operator. Case 1, the one call that
demonstrably played its audio, is incoming and takes the cycle once.

### 5e-ter. `stopTone()` on the outgoing answer path is CONDITIONAL — the strongest match for case 3

The outgoing answer runs through `Manager::peerAnsweredCall` (`manager.cpp:1855-1877`) on the
PJSIP/VoIP thread, **not** `Manager::answerCall` on the app thread. And there the tone stop is
guarded:

    if (isCurrentCall(call))
        stopTone();

    addAudio(call);

    if (pimpl_->audiodriver_) {
        std::lock_guard<std::mutex> lock(pimpl_->audioLayerMutex_);
        getRingBufferPool().flushAllBuffers();
        pimpl_->audiodriver_->flushUrgent();
    }

If `isCurrentCall(call)` is false at that instant, **`stopTone()` never runs**. Combine that with
§5d's priority order in `getToPlay()` — telephone tone is checked *before* the call's ring buffer —
and the result is exact: the ringback tone keeps winning, `bufferPool.getData(DEFAULT_ID)` is never
reached, and the inbound audio accumulates in a buffer nobody drains for the entire call, with
continuous frame delivery and not one log line.

This is outgoing-only, it is timing-dependent (hence intermittent), and it produces precisely the
case-3 signature: real speech on the wire, nothing heard, no error anywhere. **It is the leading
hypothesis for case 3 and it is testable the moment a proper capture exists** — the `getToPlay`
tone branch and the `isCurrentCall` result both need one log line each to settle it.

Note also `flushAllBuffers()` immediately after `addAudio()`: any inbound audio already buffered at
that instant is discarded.

### 5e-quater. NEGATIVE RESULTS — three mechanisms excluded by grep

Run across all three logcats, all returning **zero** in every case:

    threadloop / "Unwaited exception"            c1=0  c2=0  c3=0
    "Failed to resample frame"                   c1=0  c2=0  c3=0
    "Infinite loop detected in audio resampler"  c1=0  c2=0  c3=0
    "Failed to initialize resampler context"     c1=0  c2=0  c3=0
    "buffer lost" / "enqueue failed"             c1=0  c2=0  c3=0
    "Enqueue silentBuf_ failed"                  c1=0  c2=0  c3=0
    "Audio file error"                           c1=0  c2=0  c3=0
    "Peer muted"                                 c1=0  c2=0  c3=0

**Excluded: the receive thread dying on a resampler throw.** A `Resampler::resample` throw
(`resampler.cpp:50`, `:114`, `:131`) would unwind through `RingBuffer::put` into `ThreadLoop`'s
catch-all (`threadloop.cpp:45-48`), killing the receive thread permanently while RTP keeps arriving
on the shared socket and the player keeps re-priming `silentBuf_`. That is a perfect match for the
symptom — and it did not happen in any of these three captures. Worth fixing anyway: the guard at
`resampler.cpp:125` is `ret & AVERROR_INPUT_CHANGED || ret & AVERROR_OUTPUT_CHANGED`, a bitwise AND
against negative constants, so a generic negative return can spuriously match and drive the reinit
recursion into the throw.

**Excluded for case 1: the stuck-tone hypothesis (§5d).** `Manager::answerCall` calls `stopTone()`
**unconditionally** at `manager.cpp:956`; `toneCtrl_.stop()` sets `currentTone_ = TONE_NULL`
(`tonecontrol.cpp:112-113`), and the only two writers of `currentTone_` sit behind the same
`getPlayTones()` guard, so there is no asymmetric hole and no re-arm path after answer. Case 1's log
shows the guard released at 746.656, 75 ms before the new player existed.

**Excluded on acoustics for every case, unless the user says otherwise.** Russian ringback is
`"425/1000,0/4000"` (`tonelist.cpp:141`) — 1 s of 425 Hz then 4 s of silence, looping. A stuck
ringback across a 21 s call is roughly four clearly audible one-second beeps. Users report that as
"гудки", not "I could not hear the operator". **One question to kate settles §5d and §5e-ter for
cases 2 and 3: on the bad calls, did you hear repeating beeps, or nothing at all?**

### 5e-quinquies. CONFIRMED FROM THE FIELD LOG: `ringtoneEnabled` is false, so the destroy/rebuild happens on EVERY incoming call

`Manager::playRingtone` (`manager.cpp:2004-2035`) reaches `ringback()` — and therefore the PLAYBACK
guard at `manager.cpp:367-368` — by one of two routes: `!account->getRingtoneEnabled()` at `:2026`,
or `toneCtrl_.setAudioFile()` failing at `:2033-2034`.

Case 1 rules out the second and pins the first:

- `AudioDeviceType` is `{ ALL = -1, PLAYBACK = 0, CAPTURE, RINGTONE }` (`audiolayer.h:66`), so the
  answer-time line `Stopping OpenSL audio layer for type 0` is unambiguously **PLAYBACK**.
- The pre-answer tone opens as a playback stream 27 ms after the INVITE, with no RINGTONE stream
  anywhere in the log:

      744.958  sipcall.cpp:132       [call:...] Create a new [INCOMING] S...
      744.977  opensllayer.cpp:175   OpenSL init started
      744.985  opensllayer.cpp:62    Start OpenSL audio layer
      744.985  audio_player.cpp:105  Creating OpenSL playback stream {s16, 1 channels, 4800...
      744.992  audio_player.cpp:200  OpenSL playback start

- `ToneControl::setAudioFile` (`tonecontrol.cpp:78-94`) logs `Audio file error: %s` on the only
  failure path (`AudioFileException`, `:89-90`). That string appears **zero** times in any log.

So `ringtoneEnabled` is false for this account in the shipped build, `ringback()` is taken every
time, and **the answer-time destroy-and-rebuild of the playback device is the normal path on every
incoming call — not an edge case.** That materially raises the importance of §5b: LIB-1's window,
where a swallowed OpenSL construction failure leaves `player_` null while `status_` says Started and
the guard refcount latches at 1, is open on *every single incoming call*.

### 5e-sexies. The preferences file fails to parse at startup

Case 1 captures app start, and it fails:

    730.789  manager.cpp:2808  Preferences node unserialize YAML exception: invalid node;
                               first invalid key: "preferences"
    730.789  manager.cpp:341   Errors while parsing /data/user/0/ru.rostelecom.vatsmobilelk/cache

`manager.cpp:2798-2816` wraps `preferences.unserialize`, `voipPreferences.unserialize`,
`audioPreference.unserialize` and `videoPreferences.unserialize` in **one** try block, so the first
throw abandons all four. Every preference silently falls back to its compiled default — including
the audio-processing settings. Cases 2 and 3 do not show this only because their logcats begin after
app init.

This is a separate defect and it should be fixed on its own: unserialize each node in its own try, and
surface the failure rather than logging one line and continuing with defaults.

### 5e. Conditional destroy/rebuild at answer — real, and it depends on which tone was armed

`Manager::answerCall` (`manager.cpp:946-972`) runs `stopTone()` at :956 then `addAudio()` at :972.
Which stream `stopTone()` releases depends on the ringing path:

- `playRingtone()` succeeded (`manager.cpp:2029-2030`) → guard is **RINGTONE** → PLAYBACK was never
  up → `addAudio` does a clean 0→1 first start. No rebuild.
- ringtone disabled or `toneCtrl_.setAudioFile()` failed (`manager.cpp:2033-2034` → `ringback()` →
  `playATone` → `manager.cpp:367-368`) → guard is **PLAYBACK** → `stopTone()` drives 1→0 →
  `stopStream(PLAYBACK)` destroys the player (`opensllayer.cpp:124-129`) → `addAudio` 0→1 rebuilds
  it, straddling the 200 OK and the app's `setMode`.

Case 1 took the rebuild branch and the rebuild **succeeded**:

    746.656  opensllayer.cpp:122   Stopping OpenSL audio layer for type 0
    746.659  audio_player.cpp:176  Destroying OpenSL playback stream
    746.731  audio_player.cpp:105  Creating OpenSL playback stream
    746.735  audio_player.cpp:200  OpenSL playback start

Note also that `opensllayer.cpp:62 Start OpenSL audio layer` fires twice in case 1 — 746.679 on
tid 4022 and 746.730 on tid 632 — and `:112 OpenSL audio layer started` likewise. Two threads drive
the audio layer concurrently through the answer, and `AudioDeviceGuard`'s decision and its
`startStream`/`stopStream` call are outside any lock (there is no `audioStreamMutex_` in this tree).

### 5f. The mid-call SSRC change is the UPLINK rebuilding — not playback. Inbound survives it.

Worth recording because it was mis-attributed during this investigation before being pinned down.

Across cases 2 and 3 the phone changes its outbound RTP SSRC mid-call while the server keeps one
SSRC throughout. It is triggered by the **200 OK**, it is **uplink only**, and the new stream is a
completely fresh RTP context — new SSRC, new sequence base, new timestamp base, and no marker bit
on the first packet:

| call | last pkt, old uplink SSRC | 200 OK | first pkt, new uplink SSRC | gap |
|---|---|---|---|---|
| 2A | `0xC35A818F` seq 2399 @ 1788269375.634304 | 375.650008 | `0x24B8C873` seq 1366 @ 1788269375.814910 | 180.6 ms |
| 2B | `0x138A42CC` @ 1788269416.568 | 416.574997 | `0x7CFB5A4E` @ 1788269416.729 | 160.4 ms |
| 2C | `0x6CF7A7FE` seq 821 @ 1788269447.069983 | 447.084600 | `0xE3F13F60` seq 2859 @ 1788269447.207707 | 137.7 ms |

**The inbound stream runs straight through that window untouched** — call C's downlink continues at
a metronomic 50 pkt/s with contiguous RTP timestamps across the gap. `io_stats` at 1 s resolution
shows flat 50 pkt/s and 10000 B/s in *every* interval of *every* call in case 2, including the three
answer instants. So this is the capture/mic pipeline being rebuilt, and it costs the far end
137-181 ms of uplink — it does not touch the operator's voice.

**Playback lifetime differs by direction.** Outgoing: the playback track is built once at the
183/early-media transition and never rebuilt or re-bound for the rest of the call. Incoming: there
is no early media (180 Ringing carries no SDP), so the pre-answer ringback track is destroyed at
answer and one new playback track is created — the §5e rebuild. Either way the track is built
**exactly once per call** and whatever routing decision it gets is permanent, which is why a lost
race in §5a cannot self-correct.

### 5g. The speaker→earpiece switch freezes the whole audio pipeline for 440 ms

Direct wire measurement, case 1 uplink `0x619ea32f`. Of 417 packets exactly one delta exceeds 30 ms:

    f 2059  sn 3461  d  19.68 ms  j  0.71  sk   12.49
    f 2060  sn 3462  d 440.01 ms  j 26.92  sk -407.52
    f 2066  sn 3463  d  19.85 ms  j 25.24  sk -407.37

Sequence numbers are contiguous (`seq_err: 0`), so the phone did not drop packets — it stopped
producing them, from 1788268747.1117 to 1788268747.5517. That window is the HAL device switch:

    747.102  out_set_parameters: usecase(1: low-latency-playback) kvpairs: routing=1
    747.106  select_devices for use case (low-latency-playback)
    747.122  disable_snd_device: snd_device(88: speaker-dmic-endfire)
    747.127  enable_snd_device: snd_device(80: dmic-endfire)
    747.456  disable_snd_device: snd_device(2: speaker)
    747.486  enable_snd_device: snd_device(1: handset)
    747.511  enable_audio_route: apply mixer and update path: low-latency-playback receiver
    747.519  select_devices: done

The sender stalls when `select_devices` starts and resumes 9 ms after it finishes. `max_skew` drops
to −407 ms at that packet and **never recovers** — the uplink runs 407 ms behind its own RTP clock
for the remaining 8.1 s of the call. The freeze ends 17.6 ms before the first inbound RTP packet
(747.569256), so the real margin between "route settled" and "first far-end sample" is 50 ms, not
the 83 ms the `enable_snd_device` line alone suggests.

This is a 440 ms bidirectional audio outage at the start of every call that switches route, and it
is a plausible contributor to "I answered and heard nothing" independent of everything else here.

### 5h. Drive-by, unchanged from revision 2 and confirmed present

`src/sip/sdp.cpp` logs `media->desc.fmt[j].ptr` — a `pj_str_t`, **not** NUL-terminated — through
`%s`. Case 1 shows the consequence verbatim: `sdp.cpp:832 ]Could not find rtpmap attribute for 8?,
trying to…`. A heap over-read in a log statement only; codec comparison uses `pj_strcmp` and is
correct. Fix: `%.*s` with `.slen`, as the sibling message two blocks down already does.

---

### 5j. THE META-FINDING: v0.13.8 cannot tell you whether it played the audio

Between `RingBuffer::put` and the Android AudioTrack there is **no log, no counter and no signal**
that distinguishes "rendered the operator's voice" from "rendered zeros". The frame counter is
defeated by `silentBuf_` (§1a). The bind succeeds either way. The player reports Started either way
(§5b). `bindCallID`'s failure path returns unbound and its caller ignores the result
(`ringbufferpool.cpp:176-186`, caller `manager.cpp:1551`). `AudioReceiveThread::process` discards
`decode()`'s return value entirely, so DecodeError, ReadError and EndOfFile are all invisible
(`audio_receive_thread.cpp:105-109`). `media_decoder.cpp:421-424` drops an out-of-range
`stream_index` and returns `Status::Success` with no log at all.

**This is why revision 2 could not close the Android question, and why revision 3 cannot close case
1.** It is not a shortage of captures; the instrument does not exist.

**Three lines would have settled all three cases from the field logs alone**, and they should go in
before the next capture:

1. In `getToPlay()` (`audiolayer.cpp:346`) — a periodic count of frames actually returned by
   `bufferPool.getData(RingBufferPool::DEFAULT_ID)`, plus which branch was taken.
2. In `RingBufferPool::bindCallID` (`ringbufferpool.cpp:178,184`) — a WARN on the failure paths.
3. In `AudioReceiveThread::process` (`audio_receive_thread.cpp:105-109`) — log `decode()`'s status.

Add to that a `silentBuf_` re-prime counter reported at `AudioPlayer::stop()` (`audio_player.cpp:87`)
and every future AudioTrack frame figure becomes interpretable instead of ambiguous.

---

## 6. What changed from revision 2

1. **iOS removed entirely.** Closed in the field.
2. **The Android dataset now contains a genuine failure case** — case 3, call 11 — which revision 2
   explicitly said it lacked. It still lacks the client logs for it.
3. **Case 2's two dead calls are server-side digital silence**, proven at byte level, not a client
   defect. Revision 2 had no case descriptions and could not have known which calls were "the bad
   ones".
4. **Case 1 is UNDETERMINED, and revision 2's reasoning about it was invalid.** Both revisions leaned
   on "409728 frames delivered onto a settled, unmuted earpiece". §1a shows the OpenSL callback
   re-primes the device with zero-filled buffers whenever the upper layer starves, so that frame
   count is compatible with 8.5 s of silence. Case 1 is back on the table as a client failure, and it
   is the call that lost the mode race.
5. **The peer-number correlation is new** and is the strongest signal in the dataset.
6. **The mode race is confirmed on the capture path too**, and its owner is identified precisely:
   one unsynchronized `setMode` in `EngineService.java:399`.
7. **Three new library defects** documented with verified line numbers: §5b, §5c, §5d.
8. **`76d33574e` remains absent** from v0.13.8, so §5e's rebuild is live on this tree. But since no
   rebuild failure appears in any log, cherry-picking it is no longer the headline recommendation
   revision 2 made it.
9. **The race was caught both winning and losing** (§5a) on a ~10-15 ms margin, with the 71 ms
   main-thread dispatch hop measured as the dominant fixable contributor.
10. **The mid-call SSRC change is the uplink rebuilding at 200 OK, not playback** (§5f). It costs the
   far end 137-181 ms and leaves the inbound stream untouched.
11. **The route switch freezes the pipeline for 440 ms** (§5g), measured on the wire, uplink skew
   never recovering for the rest of the call.
12. **`mediaRestartRequired_` is dead** (§5e-bis) — initialised true, never cleared, so every SDP
   negotiation tears down and rebuilds all media. Outgoing calls with early media do it twice.
13. **`stopTone()` is conditional on the outgoing answer path** (§5e-ter). Combined with the tone
   priority in §5d this is the first mechanism that matches case 3 exactly, end to end, in code.

## 7. What the evidence still cannot show

- Whether case 3's client had a player at all, what output it landed on, whether a tone was still
  winning the priority race, or whether the ring buffer was drained. **All of it. There are no
  client logs for that call.** §5e-ter is the leading hypothesis and it is one log line from being
  settled.
- Whether `isCurrentCall(call)` was true at `manager.cpp:1862` on the failing call. This single
  boolean decides whether the ringback tone was ever stopped, and therefore whether the operator's
  voice could ever reach the speaker.
- **Whether case 1 failed at all.** After excluding the resampler throw, LIB-1, the stuck tone and
  the lost binding, no mechanism in the tree fits case 1's evidence. Every observable in that log is
  equally consistent with the call having played correctly. The honest verdict is *undetermined*, and
  §5j explains why no amount of re-reading closes it.
- Whether the `89963540961` leg's silence originates at the media server, at a transcoder, or at the
  far-end carrier. That requires server-side capture, which is outside this repo.
- Whether §5d's tone mechanism is audible-beeping or silent when it fires.

---

## 8. Ranked candidates and the minimal v0.13.8 fixes

Ranked by how well each explains the evidence actually captured, not by how interesting it is.
Everything here is verified on `00251ef8` with line numbers; nothing here is yet proven to have
caused a field incident.

| # | mechanism | where | status against the field logs | minimal fix |
|---|---|---|---|---|
| 1 | Redundant media rebuild at the 200 OK | `sipcall.h:398`, `sipcall.cpp:2213,2258` | `mediaRestartRequired_` never cleared, so it fires on every negotiation — twice per outgoing call | Suppress the rebuild when the negotiated media is unchanged. **Highest leverage: it closes the window candidates 2, 3 and 5 all depend on.** |
| 2 | Latched `startStream(PLAYBACK)` failure | `opensllayer.cpp:64-80`, `:113` | **EXCLUDED** — its `SIP_CORE_ERR` never appears, and that macro demonstrably logs in this build | Don't set `status_` on failure; hold a scoped PLAYBACK guard across `stopTone()` at `manager.cpp:956` and `:1862` |
| 3 | `AudioDeviceGuard` start/stop not serialised | `manager.cpp:2176-2196` | Reachable — case 1 shows the 632/4022 interleave — but it did not land that way | One mutex around both guard bodies |
| 4 | Resampler throw kills the receive thread | `resampler.cpp:125,131` → `threadloop.cpp:45-48` | **EXCLUDED** by grep, all three logs | try/catch in `RingBuffer::put` that drops the frame; change `ret & AVERROR_*` to `ret == AVERROR_*` at `:125` |
| 5 | Tone arm/disarm predicate mismatch | `manager.cpp:1885-1886` vs `:1861-1862` | Outgoing only; excluded unless kate reports beeping | Make `stopTone()` unconditional at `:1862` |
| 6 | OpenSL callback `try_lock` bail desyncs the shadow queue | `audio_player.cpp:48-50` | Not observed; produces a silent stall with zero logs | Blocking lock; move `callback_()` out from under `m_` |
| 7 | Peer `mute_state=1` discards every decoded frame | `audio_receive_thread.cpp:60-65`, `sipvoiplink.cpp:1272-1285` | **EXCLUDED** — `Peer muted` never appears | Cheap to keep excluded: it is visible straight from the SIP |
| 8 | `answerCall` short-circuit when never marked RINGING | `manager.cpp:950-953`, `:2573-2574` | Not observed; signature is `Answer call` with no following `Add audio to call` | Do not return early without `addAudio()` |
| 9 | Six preference setters rebuild the AudioLayer mid-call | `manager.cpp:2399-2505`, `:2515-2524` | Not observed; inherits #2's swallow | Check the restart result; null-check `audiodriver_` at `:2433` |
| 10 | Unchecked `hw_infos[0]`/`[1]` | `opensllayer.cpp:176-180` | Latent; sane values logged in case 1 | Guard `size() >= 2`, fall back to defaults |
| 11 | Preferences YAML parse abandons all four nodes | `manager.cpp:2798-2816` | **OBSERVED in case 1** | One try per node; surface the failure |
| 12 | `%s` on a non-NUL-terminated `pj_str_t` | `sdp.cpp:832` | **OBSERVED in all three** | `%.*s` with `.slen` |

### Refuted outright

**Prior-art §4's mechanism — "the rebuild loses the ringbuffer binding" — is wrong.**
`AudioRtpSession::stop()` (`audio_rtp_session.cpp:222-248`) never touches `ringbuffer_`; the pool
holds the buffer strongly via `readBindingsMap_` (`ringbufferpool.cpp:154`); `stopStream`'s `flush()`
(`opensllayer.cpp:156`) only advances read offsets (`ringbuffer.cpp:69-74`); and
`AudioReceiveThread::setup` re-binds at `audio_receive_thread.cpp:96` after every restart — which is
exactly the 747.570 line in case 1. The binding survives the rebuild. This hypothesis was pursued
hard in this investigation and it does not hold.

### If you fix only three things

1. **`EngineService.java:399`** — set `MODE_IN_COMMUNICATION` at accept, before the JNI call. Closes
   §5a, the one defect confirmed to be firing in the field.
2. **`sipcall.cpp:2258`** — stop rebuilding media when nothing changed. Closes the answer-time window
   that most library candidates depend on.
3. **The three log lines in §5j** — so the next capture can actually answer the question.

---

# Revision 4 — the mechanism is proven on the wire

**Revision 4 supersedes revision 3's ranking.** Candidate #1 from §8 ("redundant media rebuild at
the 200 OK") is no longer a candidate. Two new captures plus a five-call control set prove it,
identify the trigger that decides whether a call is affected, and explain every field report
collected so far except case 6.

New evidence: `logs/android_no_sound_incoming/5/` and `/6/` — PCAPdroid capture plus Android Studio
logcat for each, device Xiaomi MI 6 / Android 9, app `ru.rostelecom.vatsmobilelk`, user `kate`,
domain `TDEIP.21.rt.ru`, library `Svetets SIP core 0.13.8.1 (android)` (this branch, with
`411e45909` already applied). Control set: `logs/android_no_sound_incoming/4/PCAPdroid_02_сент._18_04_52.pcap`,
five outgoing calls from user `n.plaksin` through gateway `95.167.42.116`, all reported good.

## 9. Case 5 — the failure, proven end to end

Outgoing call `kate` → `+7 411 231-84-21`, Call-ID `ZQBDqCMYMvUGDWr3qus-0A4C.44-BG4M`,
peer UA `RTC CallManager 9.2-300`. The user heard the far side; the far side heard nothing.

### 9.1 What the SIP dialog did

The gateway answered the INVITE with **183 Session Progress carrying SDP**, and only 11 s later
with the real 200 OK. pjsip runs a complete offer/answer for each, and `sdp_media_update_cb`
(`sipvoiplink.cpp:1151-1192`) calls `SIPCall::onMediaNegotiationComplete()` unconditionally after
each one. Both are visible in the logcat:

```
1788422205.263  sipcall.cpp:1521  onEarlyAnswered() - 183 with SDP
1788422205.263  inv0x6fda9ff8a8   SDP negotiation done: Success        <- negotiation 1
1788422216.300  inv0x6fda9ff8a8   Got SDP answer in Response msg 200/INVITE/cseq=10811
1788422216.300  inv0x6fda9ff8a8   SDP negotiation done: Success        <- negotiation 2
```

### 9.2 What the second negotiation did to the media

`onMediaNegotiationComplete()` (`sipcall.cpp:2264-2295`) gates its whole body on
`mediaRestartRequired_`, which is initialised `true` at `sipcall.h:405` and is only ever *set*
again at `sipcall.cpp:2234` — never cleared. So the second negotiation takes the full restart path:

```
1788422216.300  sipcall.cpp:2269       Media negotiation complete
1788422216.300  sipcall.cpp:1765       updating negotiated media
1788422216.301  sipcall.cpp:1921       Stopping all media
1788422216.311  socket_pair.cpp:191    Instance destroyed              <- UDP socket 57588 CLOSED
1788422216.322  sipcall.cpp:1855       Starting all media              <- 11 ms unbound
1788422216.322  socket_pair.cpp:308    Creating rtp socket for uri rtp://212.122.2.180:9952 on port 57588
1788422216.323  media_encoder.cpp:107  New instance created            <- fresh ffmpeg rtp muxer
```

The rebuild produced **byte-identical parameters** to the first one. Compare the two socket lines:

```
1788422205.301  Creating rtp socket for uri rtp://212.122.2.180:9952 on port 57588   (after the 183)
1788422216.322  Creating rtp socket for uri rtp://212.122.2.180:9952 on port 57588   (after the 200 OK)
```

Same remote address, same remote port, same local port, same codec (`Found that payload 804 can be
PCMA 8000` in both). Nothing needed to change. The teardown was pure loss.

### 9.3 What the gateway saw

`tshark` over the two packets that straddle the rebuild:

```
t=28.5928   ssrc=0xa782632b   seq=1927   ts=3435630548
t=28.6745   ssrc=0xb27e4521   seq=1829   ts=2017976321   marker=0
```

Four simultaneous discontinuities on one 5-tuple:

| property | before | after | delta |
|---|---|---|---|
| SSRC | `0xa782632b` | `0xb27e4521` | changed |
| sequence | 1927 | 1829 | **−98 (backwards)** |
| RTP timestamp | 3 435 630 548 | 2 017 976 321 | **−1 417 654 227** |
| marker | — | 0 | no resync signal |

Plus an 82 ms packet gap, of which ~11 ms had the local port unbound (any gateway RTP arriving in
that window draws an ICMP port-unreachable).

The gateway kept **one** SSRC `0xd6dd76b2` for the whole 24 s and never stopped sending to us —
which is why the user could hear the far side throughout. It simply stopped relaying our audio
onward to the PSTN leg.

### 9.4 Why the numbers look the way they do

`initSeqVal_` exists on this base and is plumbed all the way to the ffmpeg muxer
(`media_encoder.cpp:241`, `av_opt_set_int(outputCtx_, "seq", …)`), but it is only refreshed inside
`AudioRtpSession::startSender()`:

```cpp
// src/media/audio/audio_rtp_session.cpp:127-129
socketPair_->stopSendOp();
if (sender_)                                      // <-- only when the sender survived
    initSeqVal_ = sender_->getLastSeqValue() + 1;
```

A full `stop()` (`audio_rtp_session.cpp:222-248`) already did `sender_.reset()`, so on the restart
path `sender_` is null, `initSeqVal_` keeps its stale value, `setInitSeqVal()` skips the option
(`if (seqVal != 0)`), and `rtp_write_header()` falls back to
`s->seq = av_get_random_seed() & 0x0fff` (`libavformat/rtpenc.c:92`) — hence a *low* number,
1829, which happens to land below 1927. The SSRC and the timestamp base take the same random path
(`rtpenc.c:74-78`); neither is pinned anywhere in this tree.

## 10. The control experiment — five good calls, one variable

`logs/android_no_sound_incoming/4/PCAPdroid_02_сент._18_04_52.pcap` holds five outgoing calls the
user reported as good, three of them to the same mobile number that fails elsewhere:

| CSeq | callee | provisional responses | negotiations | our SSRCs | packets |
|---|---|---|---|---|---|
| 1497 | 009 | 100 → 401 → 100 → 200 | 1 | `0x3577050c` | 51 |
| 16599 | 89963540961 | 100 → 401 → 100 → **180** → 200 | 1 | `0x75a81c6b` | 727 |
| 5178 | 89963540961 | 100 → 401 → 100 → **180** → 200 | 1 | `0xd89876b2` | 742 |
| 631 | 009 | 100 → 401 → 100 → 200 | 1 | `0x74b1db93` | 278 |
| 8096 | 89963540961 | 100 → 401 → 100 → **180** → 200 | 1 | `0x9ebca4d7` | 2079 |

**Not one of them received a 183 with SDP.** `180 Ringing` carries no SDP, so pjsip runs a single
offer/answer, `onMediaNegotiationComplete()` fires once, and the stream keeps one SSRC for its whole
life — including on calls to a mobile through the PSTN.

The single variable that separates every working call from the failing one is whether the trunk
answers with **183 Session Progress carrying SDP**. That is also why the symptom is trunk-dependent
rather than device-dependent, why pure SIP↔SIP calls are unaffected, and why it is intermittent
from the user's point of view.

## 11. Why pjsip negotiates twice at all

The second negotiation is a genuine second offer/answer, not a re-delivery of the first result.
pjsip's rule is at `pjsip/src/pjsip-ua/sip_inv.c:2238-2260`: when a transaction already has
`sdp_done`, it renegotiates if the role is UAC, an earlier negotiation completed on an early
response (`done_early`), that early response was **not** reliable (`!done_early_rel`), the new
status is 2xx or 18x, and the To tag matches. `accept_multiple_sdp_answers` gates it and defaults
to `PJ_TRUE` (`pjsip/include/pjsip/sip_config.h:449-451`); nothing in this tree overrides it.

`done_early_rel` is false because the library never negotiates 100rel — `pjsip_100rel_init_module()`
is called at `sipvoiplink.cpp:734` but `PJSIP_INV_SUPPORT_100REL` is never set on an invite session,
so the 183 arrives unreliably and its answer stays provisional in pjsip's eyes. Every condition
holds for case 5, and pjsip logs it verbatim: `Received final response after SDP negotiation has
been done` → `Got SDP answer in Response msg 200/INVITE/cseq=10811` → `SDP negotiation done: Success`.

## 11a. Why iOS on modern sip_core is unaffected

`feat/17` still restarts media at the 200 OK — and it does so unconditionally. The guard at
`feat/17:src/sip/sipcall.cpp:3765` (`earlyMediaStarted_ and state == PJSIP_INV_STATE_EARLY`) reads
like a fix for this, but it is dead code: `earlyMediaStarted_` can only be set through
`startEarlyMediaLocked()`, which is gated on `earlyMediaRequested_`, which is set only in
`SIPCall::onEarlyMediaProgress183()` (`sipcall.cpp:2727`) — and **that function has no caller
anywhere in the tree**. Commit `099400be9` ("fix: 183 early as CURRENT") rewired the PROGRESS
handler to `onEarlyAnswered()` and orphaned the whole early-media path. So `feat/17` runs
`setupNegotiatedMedia(); stopAllMedia(); updateRemoteMedia(); startAllMedia();` twice, exactly like
0.13.8.

What saves it is two mechanisms in the media layer that 0.13.8 does not have:

- **The port never unbinds.** `RtpSession::preserveCurrentSocketPairReservationIfNeeded()`
  (`feat/17:src/media/rtp_session.h:121`) pulls the bound file descriptors out of the `SocketPair`
  into a `ReservedSocketPair` *before* `socketPair_.reset()`, and the new `SocketPair` is
  constructed from that reservation. Introduced by `a996bc007` "feat: new port allocation
  mechanism" and wired into the audio stop path by `304ced63c`.
- **The sequence number continues.** `feat/17:src/media/audio/audio_rtp_session.cpp:467` saves
  `lastSenderSeqVal_ = sender_->getLastSeqValue()` in `stop()`, and the next `startSender()` seeds
  the muxer with `*lastSenderSeqVal_ + 1`. Introduced by `607fa3e06`.

Commit `4a0b2679e`'s message names this bug in so many words: *"a double stop/restart cycle that
closed the RTP port briefly, making some SIP servers stop sending RTP after receiving ICMP
port-unreachable."*

Neither branch pins the SSRC or the RTP timestamp base — no `av_opt_set_int(…, "ssrc", …)` exists
anywhere in `src/`, and `feat/17:src/media/socket_pair.h:416-418` states outright that the SSRC
"changes whenever the FFmpeg muxer is recreated, e.g. on a sender restart". So on `feat/17` the SSRC
does still change at the 200 OK, and iOS is nevertheless reported 100 % good. That places the
decisive factors on the port staying bound and the sequence staying monotonic, not on SSRC identity.

### What the ffmpeg muxer will and will not let us pin

`libavformat/rtpenc.c` in the prebuilt ffmpeg-n5.0 exposes five AVOptions — `rtpflags`,
`payload_type`, `ssrc`, `cname`, `seq` — so **SSRC and sequence can be pinned from C++ with no
contrib rebuild**. The RTP timestamp base cannot: `rtp_write_header()` does
`s->base_timestamp = av_get_random_seed();` at `rtpenc.c:74` and there is no option for it. Pinning
the SSRC while the timestamp base still jumps would produce a stream that is *invalid* rather than
merely new — one synchronisation source whose clock leaps backwards by 1.4 billion ticks. That rules
out "restart but fake continuity" as a complete answer and is why the fix below removes the restart
instead.

## 12. Case 6 — no client-side media defect

Incoming call from `79963540961` → `kate`, Call-ID `ynhc-bjPXQ1ZqXFFH@10.243.195.12`, same peer UA.
As the UAS the app negotiates once, and the logcat shows exactly one `Media negotiation complete`
(`1788422437.542`) and one socket creation (`rtp://212.122.2.180:29132 on port 60622`).

On the wire the outgoing stream is textbook: one SSRC `0x4d0b0854`, 637 packets, sequence 223…860
strictly +1, timestamps strictly +160, `lost: 0`, and sampled payloads carry real speech, not `0xD5`
digital silence. **The folder-5 mechanism does not apply here and must not be claimed for it.**

Two observations recorded without a conclusion:

- Our `200 OK` was retransmitted at 0.5, 1, 2, 4, 4, 4, 4, 4 s — the far side's `ACK` never reached
  us — while the gateway did start sending RTP, so it had received the 200 OK. The `Contact` we
  offered is `<sip:kate@192.168.31.104:42751>`, a private LAN address.
- Both captures were taken through PCAPdroid's VPN, so the app bound its SDP to the tun address
  (`c=IN IP4 10.215.173.1`) rather than its Wi-Fi address. That is a capture artifact and is absent
  from the field reports in cases 1–3, which show the same symptom.

## 13. What revision 4 changes in the ranking

- §8 candidate **#1** is promoted from candidate to **confirmed root cause for the outgoing-call,
  183-with-SDP scenario**, with the wire evidence in §9.3 and the control set in §10.
- `411e45909` ("don't mark a call CONNECTED on 183") is **correct but not the audio fix**. The new
  logcat shows `Peer answered` now firing at both `1788422205.263` and `1788422216.327`, exactly as
  designed — and the media still rebuilds. It should stay; it does not close this defect.
- The `AudioDeviceGuard` / OpenSL candidates (§8 #2, #3, #6, #9) are unchanged and untouched by this
  evidence. They remain open for the *incoming*-call reports in cases 1 and 6.

## 14. The fix

Two changes, both on `research/android-nosound-v0.13.8`.

### 14.1 Do not restart media when the negotiation changed nothing (primary)

`SIPCall::setupNegotiatedMedia()` now returns a digest of the media it applied, built purely from
`Sdp::getMediaSlots()`. `startAllMedia()` records that digest in `startedMediaFingerprint_` when it
actually starts something; `stopAllMedia()` clears it. `onMediaNegotiationComplete()` compares the
two and skips `stopAllMedia()/startAllMedia()` when they match.

Result on the case-5 call flow: the 183 starts media and records the digest, the 200 OK negotiates
the same media, the digest matches, and the RTP session is left alone. One SSRC, one sequence space,
one timestamp base, and the socket never closes.

**The digest is SDP-only, deliberately.** The obvious implementation — compare the freshly negotiated
`MediaDescription` against the RTP session's live `send_`/`receive_` — cannot work on this codebase,
and three independent reviews converged on the same proof. `AudioRtpSession::startSender()` writes
`send_.fecEnabled = true` (`audio_rtp_session.cpp:124`) *after* `configureRtpSession()` stored the
negotiated description, and `Sdp` never assigns `fecEnabled` at all, so a fresh description always
carries the `{false}` default. Running `true` vs fresh `false` → the gate would never fire, on the
one call flow it was written for. Video is worse: `VideoRtpSession::startSender()` writes
`send_.linkableHW` and `send_.bitrate` (`video_rtp_session.cpp:258-259`), and `send_.bitrate` is
rewritten continuously by congestion control on `rtcpCheckerThread_`. `RtpSession::send_` is working
state, not a record of the negotiation. Both sides of this comparison come from the SDP, so none of
that is visible to it.

Two further details the reviews forced:

- **One read of the active SDP per negotiation.** `Sdp::activeLocalSession_` / `activeRemoteSession_`
  are raw `pjmedia_sdp_session*` written on the PJSIP thread (`sdp.cpp:138,146`) and read on the
  event thread (`sdp.cpp:916`), with no lock. Computing the digest separately would have read them
  twice, letting the decision and the thing it is applied to come from different sessions — and
  would have doubled the `pj_pool` churn, since `getMediaSlots()` allocates a 4 KB pool per local
  media via `getFilteredSdp()`. Returning the digest from `setupNegotiatedMedia()` keeps it at one
  read, the same as before this change.
- **`receiving_sdp` is digested from its first `m=` line only.** It is a re-print of the whole local
  session; pjsip hands out a fresh active local session per negotiation and may re-version its `o=`
  line, which would make the digest differ for a cosmetic reason and silently disable the gate. The
  session-level lines it drops (`v= o= s= c= t=`) are either counters or already covered by
  `addr`/`rtcp_addr`.

When the digests differ and media was running, the two digests are logged at DBG. A gate that never
fires must not be indistinguishable from a gate that is not there.

### 14.2 Carry the RTP sequence across a genuine restart (safety net)

`initSeqVal_` was only refreshed while the old sender was still alive
(`audio_rtp_session.cpp:128-129`, `if (sender_)`), which is never true after a `stop()`. So a real
restart fell through to `MediaEncoder::setInitSeqVal(0)` → skipped → ffmpeg's
`av_get_random_seed() & 0x0fff`. `AudioRtpSession::stop()` now stashes
`lastSenderSeqVal_ = sender_->getLastSeqValue()` before dropping the sender, and `startSender()`
seeds from it. Wrap-around to 0 is bumped to 1 because `setInitSeqVal()` treats 0 as "unset".

This is `607fa3e06`'s mechanism, extracted by hand — that commit is 1831 insertions across 20 files
and does not cherry-pick onto this base. It does not fix case 5 on its own (the SSRC and timestamp
would still jump); it makes hold/unhold, codec change and connectivity re-INVITE cheaper for the
peer, and it is what remains if the primary gate ever declines to fire.

### 14.3 What was deliberately not done

- **The port reservation from `a996bc007` + `304ced63c`.** It is the other half of what makes
  `feat/17` survive, but it is 790 insertions across 13 files and changes `SocketPair`'s constructor
  from `(uri, int localPort)` to `(uri, ReservedSocketPair&&)`, with the reserved port having to be
  published in the SDP offer. `git merge-tree` shows conflicts in `CMakeLists.txt`,
  `audio_rtp_session.cpp`, `video_rtp_session.cpp`, `sipcall.h` and `sipvoiplink.cpp`. With §14.1 in
  place the socket is not destroyed on this call flow at all, so the window it closes does not open.
- **Pinning the SSRC.** Possible without a rebuild, but not safe alone — see §11a.
- **Video.** `VideoRtpSession` keeps its own `initSeqVal_` path and is untouched. Video is not on the
  PSTN call flow this fixes, and its `rtcpCheckerThread_` rebuilds the sender on its own schedule
  whenever congestion control moves the bitrate, so wire continuity there is not something this
  change can promise.

### 14.4 How to verify from a capture

Place an outgoing call through the `TDEIP.21.rt.ru` trunk to a mobile — one that answers with 183
Session Progress carrying SDP. Then:

1. `logcat | grep -E "Media negotiation complete|Negotiated media"` must show two
   `Media negotiation complete` lines and, on the second, `Negotiated media is unchanged, keeping
   the running RTP session`. If it instead prints `Negotiated media changed, restarting`, the two
   digests are in the same log line — diff them to see which field moved.
2. `socket_pair.cpp:308 Creating rtp socket` must appear **once**, not twice.
3. In the pcap, `rtp.ssrc` for our source port must have exactly one value for the whole call, and
   `rtp.seq` must be strictly +1 across the 200 OK.
4. Expect a short local audio discontinuity at the 200 OK regardless: `Manager::peerAnsweredCall()`
   runs a second time and calls `flushAllBuffers()` + `flushUrgent()` (`manager.cpp:1885-1889`).
   That flushes ring buffers, not the RTP stream — the wire assertions above still hold. This is the
   same thing every non-183 call already does at answer time.

Case 6's shape (incoming, one negotiation) is unaffected by construction: pjsip's multiple-answer
branch requires `tsx->role == PJSIP_ROLE_UAC` (`sip_inv.c:2245`).

---

# Revision 5 — the field verdict, and a correction

**Revision 4's §13 named the double media rebuild as *the* confirmed root cause. That is now
downgraded.** The build that was tested against production and reported working carries two changes,
and the one that fixed the calls is the other one.

## 15. The RTP header extension was the cause

The library's bundled ffmpeg is patched to add an RFC 8285 one-byte header extension carrying
abs-send-time to every outgoing RTP packet (`contrib/src/ffmpeg/rtp_ext_abs_send_time.patch`,
`#define EXT_ABS_SEND_TIME` in `libavformat/rtpenc.c`). The `RTC CallManager` media gateway in front
of the PSTN mishandles it. The patch now compiles the extension out:

```c
#undef EXT_ABS_SEND_TIME /* off: the RT trunk mishandles the 0xBEDE ext; feat/17 sends none */
```

The capture from the failing build confirms the library was sending it on every packet — all 1193
of our RTP packets in `logs/android_no_sound_incoming/5/` carry `rtp.ext == True` with
`rtp.ext.profile == 0xbede`. With the extension gone, calls through the trunk work; verified on
production on arm64.

That also settles a question revision 4 left open. `feat/17` was never affected because it sends no
header extension at all — which is exactly what `feat/17:src/media/socket_pair.h` documents and what
the `#undef`'s own comment records.

## 16. What this means for §9–§14

- **§9's mechanism is real but was not what broke the calls.** The SSRC/sequence/timestamp reset at
  the 200 OK is measured, reproducible and still in the captures. It simply was not the thing the
  gateway choked on.
- **§10's control experiment is weakened.** The five good calls in folder 4 went through a different
  gateway (`95.167.42.116`), so they differ from case 5 in *two* variables — no 183-with-SDP **and**
  a different media plane. The correlation with 183 stands, but it is no longer a clean single
  variable.
- **§14's fix stays.** Not restarting media when the negotiation changed nothing is correct on its
  own terms: it removes a needless SSRC/sequence/timestamp discontinuity and an unbind window on
  every early-media call. It ships alongside the extension change, and the production build that was
  verified contains both. It is not, on this evidence, load-bearing.
- **§13 is superseded.** Candidate #1 from §8 is a real defect that has been fixed; it is not the
  root cause of the no-sound reports.

## 17. Shipped

`sip_core 0.13.8.3` / `ru.svetets.sip:android-sip-core-wrapper:1.8.8-fix.2`, containing:

1. `#undef EXT_ABS_SEND_TIME` — the fix that production verified.
2. §14.1 — no media rebuild when a second negotiation changes nothing.
3. §14.2 — RTP sequence carried across a genuine restart.
4. `411e45909` — do not mark a call CONNECTED on 183 (from the previous round; correct, not the
   audio fix).

All four Android ABIs are built against ffmpeg trees rebuilt from the patched source; verified by
extracting `rtpenc.o` from each `libavformat.a` and confirming zero occurrences of the `0xBEDE`
constant.
