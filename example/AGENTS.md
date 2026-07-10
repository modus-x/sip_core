<!-- Parent: ../AGENTS.md -->

# example/

In-tree consumers of `sip_core`. Currently only `sip_cli/`.

## Subdirectories

| Dir         | Purpose                                                                                          | Guide                                              |
|-------------|--------------------------------------------------------------------------------------------------|----------------------------------------------------|
| `sip_cli/`  | Console example: registers an account, places/answers calls, renders video via SDL3.             | [sip_cli/AGENTS.md](sip_cli/AGENTS.md)             |

The example is **not** built by default. Configure with `-DENABLE_CLI_EXAMPLE=ON` (the `darwin`/`linux`/`win32` presets enable it) and `cmake --build out/build/<preset> --target sip_cli`.

<!-- MANUAL: -->
