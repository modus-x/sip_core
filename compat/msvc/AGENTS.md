<!-- Parent: ../AGENTS.md -->

# compat/msvc/ — MSVC compatibility shims

Headers and small C sources providing POSIX-flavored APIs that MSVC's CRT lacks. Compiled into the daemon when building with MSVC.

## Files

| File                | Role                                                                                                |
|---------------------|-----------------------------------------------------------------------------------------------------|
| `dlfcn.h`, `dlfcn.c` | `dlopen`/`dlsym`/`dlclose` implemented on top of `LoadLibrary`/`GetProcAddress`/`FreeLibrary`.   |
| `unistd.h`          | Stubs for `usleep`, `gettimeofday`, etc. Some are `static inline`; others delegate to `<windows.h>`. |
| `sys_time.h`        | `struct timeval` + `gettimeofday` declaration.                                                      |
| `config.h`          | Drop-in `config.h` for code that expects autotools-style feature macros.                            |
| `winmake.py`                 | **Main Windows build driver** invoked from the root `CMakeLists.txt` (`python compat/msvc/winmake.py --build`). Reads each dep's `package.json`, resolves build order, and orchestrates MSBuild/CMake/gcc-via-MSYS2. |
| `package.json`               | Windows build recipe for the `sip_core` top-level target itself (lists dep order for the Windows pipeline). |
| `CMakeLists.txt`    | Adds these sources to `Source_Files` when MSVC.                                                     |

## Working here

- Don't grow this directory into a "Windows utilities" dumping ground. New cross-platform helpers belong in `src/connectivity/` or a dedicated module.
- If a shim is needed only by a third-party dep (not by the daemon proper), patch the dep instead — keep the shim list minimal.

<!-- MANUAL: -->
