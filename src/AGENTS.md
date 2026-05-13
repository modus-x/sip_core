<!-- Parent: ../AGENTS.md -->

# src/

Daemon source root. The `Manager` singleton + factories live directly here; subsystem implementations live in subdirectories.

## Top-level files (the "core" layer)

| File                            | Role                                                                                          |
|---------------------------------|-----------------------------------------------------------------------------------------------|
| `manager.h/cpp`                 | **Central singleton.** Wires every subsystem; entry point for every public API call.          |
| `account.h/cpp`                 | Abstract base for accounts. `SIPAccount` derives from this.                                   |
| `account_factory.h/cpp`         | Type-keyed account registry. Templated `getAccount<T>()` / `getAllAccounts<T>()`.             |
| `account_config.h/cpp`          | Base account config (alias, enabled, hostname, codecs, etc.) serialized via `Serializable`.   |
| `account_schema.h`              | String keys for account YAML fields (`Conf::ACCOUNT_ID_KEY`, `HOSTNAME_KEY`, …).              |
| `call.h/cpp`                    | Abstract `Call` (state machine: CallState × ConnectionState). `SIPCall` derives.              |
| `call_factory.h/cpp`            | Per-link-type call registry; `newSipCall()`, `getCall<C>(id)`, lifecycle bookkeeping.         |
| `call_set.h`                    | Account-scoped collection of calls + conferences.                                             |
| `conference.h/cpp`              | Multi-party call container; participant info, video mixer wiring, voice activity, recording.  |
| `conference_protocol.h/cpp`     | JSON-over-SIP conference metadata (gated by `CONFERENCE_METADATA=ON`).                        |
| `preferences.h/cpp`             | `Preferences`, `VoipPreference`, `AudioPreference`, `VideoPreferences` (YAML-backed).         |
| `registration_states.h`         | `enum RegistrationState`.                                                                     |
| `ring_api.cpp`                  | Top-level `libsip_core::init/start/fini/version/platform/logging` implementations.            |
| `ring_types.h`                  | Common type aliases.                                                                          |
| `scheduled_executor.h/cpp`      | General task scheduler used everywhere (`Manager::scheduler()`).                              |
| `threadloop.h/cpp`              | RAII thread with start/stop/join; foundation of audio/video worker threads.                   |
| `logger.h/cpp`                  | `SIP_CORE_{DBG,INFO,WARN,ERR}` macros + log routing.                                          |
| `fileutils.h/cpp`               | Cross-platform filesystem helpers.                                                            |
| `string_utils.h/cpp`            | UTF-8/ASCII helpers, splitting, formatting.                                                   |
| `uri.h/cpp`                     | SIP URI parsing/construction.                                                                 |
| `vcard.h/cpp`                   | vCard parsing for presence/contact data.                                                      |
| `base64.h/cpp`                  | Base64 encode/decode.                                                                         |
| `transport.h`                   | Abstract `Transport` interface used by `SipTransport`.                                        |
| `observer.h`, `noncopyable.h`, `map_utils.h`, `enumclass_utils.h`, `rational.h`, `compiler_intrinsics.h`, `debug_utils.h` | Header-only utilities (observer pattern, `NON_COPYABLE`, std map helpers, etc.). |
| `buildinfo.cpp`                 | Reports `version()` / `platform()` strings (configured from `version.h.in`).                  |
| `windirent.h`, `winsyslog.h/c`  | MSVC-only Windows shims (`dirent`, syslog).                                                   |

## Subdirectories

| Dir                  | What lives there                                                                | Guide                                                    |
|----------------------|---------------------------------------------------------------------------------|----------------------------------------------------------|
| `sip_core/`          | **Public API headers** (the ABI contract).                                      | [sip_core/AGENTS.md](sip_core/AGENTS.md)                 |
| `client/`            | Bridge between facade and `Manager`; signal dispatch; video manager glue.       | [client/AGENTS.md](client/AGENTS.md)                     |
| `sip/`               | PJSIP integration: VoIPLink, Account, Call, SDP, Transport, Presence.           | [sip/AGENTS.md](sip/AGENTS.md)                           |
| `media/`             | RTP, codecs, recorder, player + audio/video subtrees.                           | [media/AGENTS.md](media/AGENTS.md)                       |
| `config/`            | YAML parser + serialization helpers.                                            | [config/AGENTS.md](config/AGENTS.md)                     |
| `connectivity/`      | IP/SIP utils, UTF-8 helpers, security memory utils.                             | [connectivity/AGENTS.md](connectivity/AGENTS.md)         |
| `im/`                | SIP MESSAGE / instant-messaging engine.                                         | [im/AGENTS.md](im/AGENTS.md)                             |

## Working in this directory

- **Most public API verbs touch four files**: a header in `sip_core/`, a thin wrapper in `client/`, a method on `Manager`, and a call-/account-specific impl in `sip/` or `media/`. Grep for an existing verb name to find the pattern.
- **Don't add new code to `Manager` reflexively.** If the work is account-scoped, push it onto `Account` / `SIPAccount`; if call-scoped, onto `Call` / `SIPCall`. `Manager` is for global coordination only.
- **`ManagerPimpl`** (defined in `manager.cpp`) hides most implementation; if you can keep new fields out of `manager.h`, do so to preserve build-time hygiene.
- **Signals**: emit via `emitSignal<libsip_core::CallSignal::X>(args...)` after including `client/ring_signal.h`. Never call the host callback directly.

## Common patterns

- `editConfig([&](AccountConfig& c) { c.x = ...; })` — guarded mutation + auto-save.
- `runOnMainThread([] { ... });` (from `manager.h`) — defer work to the scheduler thread.
- `std::shared_ptr<Call>` / `std::weak_ptr<Account>` ownership is the norm. Calls hold weak refs to accounts; accounts hold strong refs to calls via `CallSet`.

## Dependencies

- **Internal**: `sip_core/` (API surface), `client/` (signal handlers), and every other subdir.
- **External**: pjproject (PJSIP), FFmpeg (via `media/`), yaml-cpp (`config/`), fmt (logging), jsoncpp (conference metadata), webrtc-audio-processing (audio DSP).

<!-- MANUAL: -->
