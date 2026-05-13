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
| `package.json`, `winmake.py` | Boilerplate used by the contrib Windows build pipeline (kept here for proximity).          |
| `CMakeLists.txt`    | Adds these sources to `Source_Files` when MSVC.                                                     |

## Working here

- Don't grow this directory into a "Windows utilities" dumping ground. New cross-platform helpers belong in `src/connectivity/` or a dedicated module.
- If a shim is needed only by a third-party dep (not by the daemon proper), patch the dep instead — keep the shim list minimal.

<!-- MANUAL: -->
