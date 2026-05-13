<!-- Parent: ../AGENTS.md -->

# contrib/ — Third-party dependencies

Every external library `sip_core` needs is built **here**, into per-triplet subdirectories, before the main library is built.

## Layout

```
contrib/
├── bootstrap                       (shell script that creates a build-<triplet>/Makefile)
├── src/                            (per-dep recipes and patches)
│   ├── ffmpeg/
│   │   ├── rules.mak               (Unix build recipe)
│   │   ├── package.json            (Windows build recipe, driven by winmake.py)
│   │   ├── build_ffmpeg.bat
│   │   ├── *.patch                 (FFmpeg patches — opus, rtp, screen-sharing, ...)
│   │   └── SHA512SUMS              (download integrity)
│   ├── pjproject/                  (PJSIP + patches)
│   ├── opus/, x264/, vpx/, ...
│   └── main.mak, get-arch.sh, change_prefix.sh, pkg-static.sh
├── build-<triplet>/                (out-of-source build dirs, generated)
│   └── <dep>/                      (extracted + built sources per dep)
└── <triplet>/                      (final install: include/, lib/, share/, pkgconfig/)
```

Triplets are GCC-style (`x86_64-apple-darwin24.0.0`, `aarch64-linux-android`, etc.); on iOS the triplet has a custom suffix (`arm64-ios-real`, `arm64-ios-simulator`).

## Subdirectory

| Dir       | What                                                                              | Guide                                              |
|-----------|-----------------------------------------------------------------------------------|----------------------------------------------------|
| `src/`    | Recipes and patches for every dep.                                                | [src/AGENTS.md](src/AGENTS.md)                     |

## How the build is invoked

From the root `CMakeLists.txt`:

1. Determine the build machine triplet via `gcc -dumpmachine`.
2. Determine target triplets (cross-compile? multi-arch on macOS?).
3. For each target, `mkdir contrib/build-<target>` and run `contrib/bootstrap --prefix=contrib/<target> --host=<target>`.
4. Run `make` in the build dir; this consults `contrib/src/<dep>/rules.mak` for each dep.
5. On Windows, `contrib/src/winmake.py` reads `package.json` files and orchestrates the build using whatever each dep wants (gcc-via-MSYS2, MSBuild, CMake).

## Dep list (full)

| Dep                          | Used by                                                       |
|------------------------------|---------------------------------------------------------------|
| `pjproject`                  | The SIP stack (patched — see patches under `src/pjproject/`). |
| `ffmpeg`                     | All media encode/decode (patched for opus/rtp/screen).        |
| `opus`                       | Audio codec (also via FFmpeg).                                |
| `x264`                       | H.264 software encoder.                                       |
| `vpx`                        | VP8/VP9 codec.                                                |
| `bcg729`                     | G.729 codec.                                                  |
| `webrtc-audio-processing`    | NS/AEC/AGC (audio-processing/webrtc.cpp).                     |
| `yaml-cpp`                   | YAML config parsing.                                          |
| `jsoncpp`                    | Conference protocol JSON.                                     |
| `fmt`                        | Logging.                                                      |
| `portaudio`                  | Windows audio backend.                                        |
| `jack`                       | Optional JACK audio.                                          |
| `iconv`, `zlib`, `lzma`, `xml2`, `brotli`, `freetype2`, `harfbuzz`, `fontconfig`, `libpng` | Transitive (FFmpeg / video text overlays). |
| `lttng-ust`, `liburcu`       | LTTng tracing (optional).                                     |
| `pthreads`                   | Windows pthreads-win32.                                       |

> **Note:** `speex`/`speexdsp` tarballs remain in `contrib/tarballs/` but their recipe directories have been removed and these deps are no longer built. Do not restore them without updating the root `CMakeLists.txt`.

## Working here

- **Patches apply during extraction** in `rules.mak` (`patch -p1 < ...`). Add patches in numeric order if they must apply on top of one another.
- **Don't commit build artifacts** (`contrib/build-*/` and `contrib/<triplet>/`). They are gitignored.
- **Bumping a dep**: change the `URL` and `SHA512SUMS` (and `VERSION` if templated) in the dep's recipe. Test on Linux/macOS first, then Windows — Windows builds are the most fragile.
- **Adding a dep**: copy an existing dep dir as a template; add it to the `DEPS_<group>` list in `contrib/src/main.mak`; reference it from the root `CMakeLists.txt` via `find_package` or `pkg_check_modules`.

## Using prebuilt deps

Set `-DBUILD_DEPS=OFF -DPREBUILD_DEPS_PATHS=/path/to/contrib-mirror`. The directory you point at must contain per-triplet subdirs in the same layout as `contrib/<triplet>/`. This is how CI typically builds the library once contrib is cached.

<!-- MANUAL: -->
