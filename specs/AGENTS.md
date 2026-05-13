<!-- Parent: ../AGENTS.md -->

# specs/

Protocol / ABI specifications. Treat these as contracts: changes here mean coordinated changes in dependent codebases.

## Files

| File                              | What                                                                                          |
|-----------------------------------|-----------------------------------------------------------------------------------------------|
| `SIP_CORE_ABI_russian.md`         | **Russian-language.** Canonical contract for the `sip_core_bindings` C wrapper that real client apps link against. Documents 5 controllers (Client, CallController, ConfigurationController, PresenceController, VideoController) and 4 notifiers (CallNotifier, ConfigurationNotifier, PresenceNotifier, VideoNotifier). Includes per-platform packaging (DLL/.so/.dylib FAT). |

## Working here

- **Any change to a `LIBSIP_CORE_PUBLIC` signature** in `src/sip_core/*.h` must be reflected here.
- The doc is intentionally in Russian to match the bindings' primary consumer team. If you don't read Russian, run it through a translator and have a Russian-speaker review your edit before merging.
- Information blocks are typed `CHANGES` (additive/behavioral) and `IMPORTANT (WARNING)` (deprecated/constraints). Use the same format when adding entries.

## Dependencies

- **Internal**: `src/sip_core/*.h` (the surface this doc describes).

<!-- MANUAL: -->
