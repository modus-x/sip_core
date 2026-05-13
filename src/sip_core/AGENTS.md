<!-- Parent: ../AGENTS.md -->

# src/sip_core/ — Public API headers

**This is the ABI contract.** Every header here is shipped to consumers and linked against. Adding, removing, or reshaping any symbol marked `LIBSIP_CORE_PUBLIC` is an **ABI break**. Update [`specs/SIP_CORE_ABI_russian.md`](../../specs/SIP_CORE_ABI_russian.md) whenever you touch these.

The implementation of every function declared here lives in `src/client/*.cpp` (signal dispatch + `Manager` glue) — search by function name to find it.

## Headers

| File                                | Surface                                                                                                  |
|-------------------------------------|----------------------------------------------------------------------------------------------------------|
| `sip_core.h`                        | Lifecycle: `init(flags)`, `start(config_file, data_path)`, `fini()`, `initialized()`, `logging()`. Defines `CallbackWrapperBase` / `CallbackWrapper<TProto>` + `exportable_callback<Ts>(...)` + `registerSignalHandlers(...)`. Includes `def.h`. |
| `callmanager_interface.h`           | Call control: `placeCall`, `placeCallWithMedia`, `accept`, `acceptWithMedia`, `refuse`, `hangUp`, `hold/unhold`, `transfer/attendedTransfer`, `muteLocalMedia/muteRemoteMedia`, `requestMediaChange`, `answerMediaChangeRequest`, plus conference verbs (`joinParticipant`, `addMainParticipant`, `setActiveStream`, `raiseHand`, `setModerator`, …) and `playDTMF`, `sendTextMessage`, `toggleRecording`. Defines `CallSignal` struct (StateChange, IncomingCallWithMedia, MediaNegotiationStatus, ConferenceCreated, RecordingStateChanged, …). |
| `configurationmanager_interface.h`  | Accounts (`addAccount`, `removeAccount`, `setAccountDetails`, `getVolatileAccountDetails`, `sendRegister`), codecs (`get/set ActiveCodecList`), audio devices (`get/set AudioInput/Output/RingtoneDevice`, `setAudioManager`), audio processing (`set{NoiseSuppress,EchoCanceller,Agc,VAD}State`, `setWebRtcParams`), and **DSCP-like preferences** (history limit, ringing timeout, push tokens). Defines `WebRtcParams`, `TransportType`, `Message`, `AudioSignal`, `ConfigurationSignal`. |
| `videomanager_interface.h`          | Video devices (`getDeviceList`, `getCapabilities`, `applySettings`, `setDefaultDevice`, `setDeviceOrientation`), input opens (`openVideoInput`, `createMediaPlayer`), sinks (`registerSinkTarget`), recorder (`startLocalMediaRecorder`). Defines `MediaFrame` / `AudioFrame` / `VideoFrame` wrappers around `AVFrame` + `SinkTarget` + `VideoSignal` / `MediaPlayerSignal`. Heavy `#ifdef` for Android/iOS — JNI glue is here. |
| `presencemanager_interface.h`       | `publish`, `subscribeBuddy`, `subscribeToEvents`, `getSubscriptions`, `answerServerRequest`. Defines `PresenceSignal` (NotifyReceived, SubscriptionStateChanged, NewBuddyNotification, …). |
| `def.h`                             | Macros: `LIBSIP_CORE_PUBLIC`, `LIBSIP_CORE_TESTABLE`, `LIBSIP_CORE_LOCAL` (visibility), `CURRENT_FILENAME()`, `CURRENT_LINE()`. |
| `account_const.h`                   | String constants for account types (`ACCOUNT_TYPE_SIP`) and parameter keys.                              |
| `call_const.h`                      | Call-state string constants used in `StateChange` signal payloads (`"INCOMING"`, `"RINGING"`, …).        |
| `media_const.h`                     | `MediaAttributeKey::*` / `MediaAttributeValue::*` / `Media::MediaNegotiationStatusEvents` namespaces.    |
| `presence_const.h`                  | Presence event-name and field constants.                                                                 |
| `device_const.h`                    | Device-property keys used in `getDeviceParams` / `applySettings`.                                        |
| `trace-tools.h`                     | LTTng-style tracepoint helpers (`sip_core_tracepoint`, `demangle<T>()`). No-op on platforms without LTTng. |
| `tracepoint.h`, `tracepoint-def.h`, `tracepoint.c` | LTTng tracepoint declarations + emission. C file is compiled into the library. |

## Working in this directory

- **Treat every change as a wire-format change.** Bump version and document in `specs/SIP_CORE_ABI_russian.md`.
- **Never expose PJSIP or FFmpeg types directly** — wrap them (see `MediaFrame` for the FFmpeg pattern). The single exception is `pjsip_evsub_state` already exposed in `CallSignal::TransferStateChange`.
- **Callback ABI uses the `CallbackWrapper<TProto>` template**, not function pointers. New signals follow the `struct LIBSIP_CORE_PUBLIC SignalName { static const char* name; using cb_type = void(...); }` pattern.
- **Deprecate, don't remove.** Functions like `registerCallHandlers` are marked `[[deprecated(...)]]` rather than deleted to give consumers time to migrate.
- **Includes are ordered for PJSIP compatibility.** Don't run `clang-format` with `SortIncludes: true` on these files.

## Signal taxonomy

Each `*Signal` struct collects every callback type for that area. To wire one up host-side:

```cpp
using namespace libsip_core;
auto handlers = std::map<std::string, std::shared_ptr<CallbackWrapperBase>>{
    exportable_callback<CallSignal::StateChange>(
        [](auto accId, auto callId, auto state, int code) { /* … */ }),
    exportable_callback<CallSignal::IncomingCallWithMedia>(
        [](auto accId, auto callId, auto from, auto media, auto hdrs) { /* … */ }),
};
registerSignalHandlers(handlers);
```

Emission inside the daemon goes through `emitSignal<CallSignal::StateChange>(accId, callId, state, code)` — see `src/client/ring_signal.h`.

## Dependencies

- Pulls in PJSIP headers (`pjsip-simple/evsub.h` etc.) — keep them visible since consumers may need the same enum values.
- Pulls in FFmpeg forward declarations (`AVFrame`, `AVPacket`) — full FFmpeg headers are NOT exposed.

<!-- MANUAL: -->
