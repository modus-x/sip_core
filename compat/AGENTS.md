<!-- Parent: ../AGENTS.md -->

# compat/ — Cross-platform shims

Tiny compatibility layer to fill missing POSIX bits on Windows. Currently only the MSVC subdirectory is meaningful.

## Subdirectories

| Dir       | What                                                                              | Guide                                              |
|-----------|-----------------------------------------------------------------------------------|----------------------------------------------------|
| `msvc/`   | Headers and stubs that MSVC needs (`dlfcn.h`, `sys_time.h`, `unistd.h`, `dlfcn.c`). | [msvc/AGENTS.md](msvc/AGENTS.md)              |

## Files

| File              | Role                                                                                              |
|-------------------|---------------------------------------------------------------------------------------------------|
| `CMakeLists.txt`  | Pulls in `msvc/` and propagates `Source_Files` to the parent.                                     |

<!-- MANUAL: -->
