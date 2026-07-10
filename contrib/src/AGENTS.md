<!-- Parent: ../AGENTS.md -->

# contrib/src/ — Per-dependency recipes

One subdirectory per third-party library, containing:

- `rules.mak` — Unix (Linux/macOS/Android/iOS) build recipe consumed by `make` in `contrib/build-<triplet>/`.
- `package.json` — Windows build recipe consumed by `winmake.py`.
- `*.patch` — patches applied during extraction.
- `SHA512SUMS` — integrity check for the download tarball.
- Optional: `build_<dep>.bat`, custom build helpers, license files.

## Top-level files

| File                  | Role                                                                                              |
|-----------------------|---------------------------------------------------------------------------------------------------|
| `main.mak`            | Master Makefile included by each generated `contrib/build-<triplet>/Makefile`. Defines `DEPS_*` groups, common targets (`extract`, `unpack`, `patch`, `download`), and host-/target-specific tool variables (CC, CXX, PKG_CONFIG, etc.). |
| `get-arch.sh`         | Helper that returns the architecture component of a triplet.                                      |
| `change_prefix.sh`    | Post-install fixup — rewrites `${INSTALL_PREFIX}` references in `.pc` and libtool files.          |
| `pkg-static.sh`       | Helper to coerce pkg-config files toward static linkage when needed.                              |
| `README`              | Brief overview of the recipe layout (English).                                                    |

## Recipes (one per dep)

| Dep                                | Notes                                                                                            |
|------------------------------------|--------------------------------------------------------------------------------------------------|
| `bcg729/`                          | G.729 codec.                                                                                     |
| `brotli/`                          | Used transitively (woff2/fontconfig path).                                                       |
| `ffmpeg/`                          | The audio/video powerhouse. Heavy patch stack — opus FEC, RTP marker, RTP ext for absolute send time, RTP DTMF events, mjpeg log spam, x11/gdigrab screen capture, iOS B-frame disable, Windows configure. |
| `fmt/`                             | `{fmt}` formatting library used by the logger.                                                   |
| `fontconfig/`, `freetype2/`, `harfbuzz/`, `libpng/` | Text rendering for video overlays (when overlays are enabled).                       |
| `iconv/`                           | Character-set conversion (Android NDK is missing one).                                           |
| `jack/`                            | Optional JACK audio.                                                                             |
| `jsoncpp/`                         | Conference-protocol JSON.                                                                        |
| `lttng-ust/`, `liburcu/`           | LTTng userspace tracing (optional, off by default in non-Linux builds).                          |
| `lzma/`                            | xz library — transitive.                                                                         |
| `opus/`                            | Opus audio codec (also via FFmpeg, kept separate for direct linking).                            |
| `pjproject/`                       | The SIP stack. Patches: Android build, Windows VS+GnuTLS, iOS PointToPoint disable, iOS 16 fixes, presence improvements, evsub header cleanup, config-site injection, Unix bzero fix. |
| `pthreads/`                        | pthreads-win32 (Windows only).                                                                   |
| `vpx/`                             | VP8/VP9 codecs.                                                                                  |
| `webrtc-audio-processing/`         | NS/AEC/AGC.                                                                                      |
| `x264/`                            | H.264 software encoder.                                                                          |
| `xml2/`                            | libxml2 — transitive.                                                                            |
| `yaml-cpp/`                        | YAML parsing.                                                                                    |
| `zlib/`                            | Compression — transitive.                                                                        |

## Working here

- **Patches**: name them with a leading number for ordering when they're not independent. Always include a comment at the top of the patch explaining why it exists — these files outlive their authors.
- **Tarball location**: the `URL` field in `rules.mak` / `package.json` points at the project's Nexus mirror (`nexus.svetlocal.ru/repository/github-artifacts/...`) for reproducibility. If you change a version, *also* update the SHA512.
- **Windows patches** are typically separated into `win_patches` in `package.json` because line-ending diffs trip up `patch.exe`.

## Dependencies between recipes

`ffmpeg/package.json` declares `"deps": ["vpx", "x264", "opus"]` so Windows builds in the right order. On Unix, `rules.mak` uses `$(DEPS_<dep>)` variables defined in `main.mak`.

<!-- MANUAL: -->
