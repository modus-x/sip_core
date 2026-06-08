<!-- Parent: ../AGENTS.md -->

# src/config/ — YAML configuration parsing

The daemon's persistent state (accounts, preferences, audio/video settings) is a single YAML file passed to `libsip_core::start(config_file, data_path)`. This directory is the parser glue.

## Files

| File                              | Role                                                                                              |
|-----------------------------------|---------------------------------------------------------------------------------------------------|
| `yamlparser.h/cpp`                | Helpers around yaml-cpp: `parseValue(node, key, T&)`, `parseValueOptional`, `parsePath`, `parseVector`, `parseVectorMap`. Used by every `*_config.cpp` to read fields. |
| `serializable.h`                  | `Serializable` interface — every config object implements `unserialize(YAML::Node)` / `serialize(YAML::Emitter)`. |
| `account_config_utils.h`          | Serialization helpers: `serializeValue<T>(out, key, value, default)` (skips writing if value equals default) and two macros `SERIALIZE_CONFIG` / `SERIALIZE_PATH` used in `serialize()` implementations to reduce boilerplate. |

## Where each config schema lives

Schemas (the actual field definitions) live next to the things they configure:

| Config object                  | Schema                                                                                          |
|--------------------------------|-------------------------------------------------------------------------------------------------|
| `Preferences`, `VoipPreference`, `AudioPreference`, `VideoPreferences` | `src/preferences.h/cpp`                                              |
| `AccountConfig` (base)         | `src/account_config.h/cpp`                                                                      |
| `SipAccountBaseConfig`         | `src/sip/sipaccountbase_config.h/cpp`                                                           |
| `SipAccountConfig`             | `src/sip/sipaccount_config.h/cpp`                                                               |

Account *attribute keys* are string constants in [`src/account_schema.h`](../account_schema.h).

## YAML structure (matches `test.yaml`)

```yaml
accounts:
  - id: <string>
    type: SIP
    hostname, username, password, transport, port, …       # SIP fields
    srtp:
      keyExchange, rtpFallback
    activeCodecs: "167/65543/65542"                        # slash-separated codec IDs
preferences:                                               # Preferences
  historyLimit, ringingTimeout, order, portNum, zoneToneChoice, …
voipPreferences:
  playDtmf, playTones, pulseLength
audio:                                                     # AudioPreference
  audioApi, captureMuted, playbackMuted,
  alsa/pulse/portaudio: { ... },
  audioProcessor, noiseReduce, echoCancel,
  voiceActivityDetection, automaticGainControl
video:                                                     # VideoPreferences
  recordPreview, recordQuality, conferenceResolution
  devices: [ { name, id, input, video_size, framerate, … } ]
```

## Working in this directory

- **Adding a new YAML field**: add to the relevant config struct, extend its `unserialize` + `serialize`, and provide a sensible default. `parseValueOptional` will not throw on missing fields — use this for backwards compatibility.
- **Account types other than SIP**: register a generator in `AccountFactory::generators_`. The code is set up for it but only `SIPAccount` is currently wired.
- **`Manager::saveConfig()`** writes the entire YAML back via `Serializable::serialize`. Make sure any field you read is also written, or it'll vanish on first save.

## Dependencies

- **Internal**: `src/preferences.h`, `src/account_config.h`, `src/sip/sipaccount_config.h`.
- **External**: yaml-cpp (`contrib/src/yaml-cpp`).

<!-- MANUAL: -->
