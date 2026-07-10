<!-- Parent: ../AGENTS.md -->

# scripts/

Dev/diagnostic helper scripts. Not part of the build or runtime.

## Files

| File                          | Role                                                                                            |
|-------------------------------|-------------------------------------------------------------------------------------------------|
| `count_slice_threads.sh`      | macOS-only. Finds the process named `svetophone` (override with `./count_slice_threads.sh <name>`), reports its thread count, and tries to estimate how many FFmpeg slice-worker threads are running. Useful when chasing a thread-leak after resolution changes during a video call. |

## Working here

- Keep scripts platform-tagged in the header (e.g. `# macOS only`).
- Bash is preferred for portability across darwin/linux; PowerShell goes in a `.ps1`.
- Don't add anything that the build pipeline needs — those belong in CMake or contrib recipes.

<!-- MANUAL: -->
