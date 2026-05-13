<!-- Parent: ../AGENTS.md -->

# tests/

Test sources. **Not currently wired into CTest** — there is no automated test target. The sources here are kept for manual compilation and as the seed for a future CTest integration.

## Files

| File                                  | What it tests                                                                                 |
|---------------------------------------|-----------------------------------------------------------------------------------------------|
| `media_recorder_tests.cpp`            | `MediaRecorder` — mux integrity, audio+video sync, restart-on-resolution-change.              |
| `sdp_offer_validation_tests.cpp`      | `Sdp` — validates offer/answer construction for various media combinations.                   |
| `sipaccount_recovery_tests.cpp`       | `SIPAccount` — main/backup route failover, OPTIONS fast-probe, connectivity recovery.         |
| `socket_pair_reservation_tests.cpp`   | `ReservedSocketPair` — port reservation survives re-INVITE.                                   |
| `video_input_tests.cpp`               | `VideoInput` — file-backed and device-backed sources, format negotiation.                     |
| `assets/test_16x12.mp4`               | Tiny 16×12 H.264 sample.                                                                      |
| `assets/test_32x24.mp4`               | Tiny 32×24 H.264 sample.                                                                      |

## Running

These tests reference internal classes (not just the public API) — they need `LIBSIP_CORE_TESTABLE` symbols exposed. Currently they have to be compiled by hand against the daemon source tree; no `CMakeLists.txt` here yet.

**To wire one of these up**:

1. Add `add_executable(<name>_test ...)` + `target_link_libraries(<name>_test PRIVATE sip_core)` to a new `tests/CMakeLists.txt`.
2. `enable_testing()` in the root `CMakeLists.txt` (already done conditionally — check).
3. `add_test(NAME <name>_test COMMAND <name>_test)`.

When adding new tests, prefer this directory for unit-level coverage and keep integration scenarios driven by `sip_cli` against a real SIP server.

## Working here

- Each test file is freestanding (no shared fixtures). If you find yourself wanting fixtures, propose a `tests/common/` header before duplicating boilerplate.
- Assets must be tiny (the two MP4s are <50 KB each). Don't add high-res samples — point at FFmpeg's own samples instead.

<!-- MANUAL: -->
