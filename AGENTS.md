# Repository Guidelines

## Project Structure & Module Organization
Core SIP logic lives in `src/` (signaling under `sip/`, media routines in `media/`, client helpers in `client/`, configuration helpers in `config/`, and platform shims in `compat/`). Dependency recipes and sources sit in `contrib/`, producing triplet-scoped outputs (for example, `x86_64-apple-darwin24.0.0`). The console example is in `example/sip_cli/`. Build metadata is at the root (`CMakeLists.txt`, `CMakePresets.json`, `.clang-format`, `.clang-tidy`, `version.h.in`). Keep generated artifacts inside `build/` (or a separate out-of-tree directory) and avoid committing them.

## Build, Test, and Development Commands
```bash
mkdir -p build && cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/opt/sip-core -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target install
cmake --build . --target sip_cli    # optional CLI example
```
Set `BUILD_DEPS=OFF` with `-DPREBUILD_DEPS_PATHS=/path/to/triplets` to reuse prebuilt binaries. Toggle `ENABLE_VIDEO` or `ENABLE_CLI_EXAMPLE` as needed. On Windows, add `--A x64 -Thost=x64` during configure. Ensure `CMAKE_PREFIX_PATH`/`CMAKE_FIND_ROOT_PATH` point to the intended contrib triplet so linking uses the packaged dependencies.

## Coding Style & Naming Conventions
Target C++17 with 4-space indents, no tabs, and a 100-character line limit. Braces wrap on new lines for classes/functions; includes are intentionally not auto-sorted. Run `clang-format -i <file>` and `clang-tidy -p build <file>` (configs provided) before submitting. Prefer descriptive CamelCase for types and existing snake_case patterns for files/functions to match the current codebase.

## Testing Guidelines
An automated suite is not yet committed; add unit or CTest coverage alongside new modules when possible. At minimum, build both Debug and Release configurations and sanity-check `sip_cli` with a local config (see the structure in `test.yaml`). Document platform-specific checks (Darwin/Linux/Windows) performed for a change.

## Commit & Pull Request Guidelines
Git history uses Conventional Commit prefixes (`feat:`, `fix:`, `chore:`). Keep messages imperative and scoped (e.g., `fix: handle null device name`). PRs should summarize the change, list build options and platforms tested, and include logs or screenshots for behavioral updates. Link issues/tasks when available and call out new dependencies or CMake options introduced.
