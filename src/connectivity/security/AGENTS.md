<!-- Parent: ../AGENTS.md -->

# src/connectivity/security/

Small helpers for handling sensitive bytes (credentials, crypto material) safely.

## Files

| File              | Role                                                                                                 |
|-------------------|------------------------------------------------------------------------------------------------------|
| `memory.h/cpp`    | Constant-time `memcmp` (`compareMemory`), and `secureZeroMemory` (compiler-defeating zeroing). Use these instead of stdlib equivalents anywhere a password, key, or HMAC is being compared or wiped. |

## When to use

- Comparing two HMACs, SDES master keys, or password hashes → `compareMemory` (avoids timing oracle).
- About to drop a `std::string` that held a password → `secureZeroMemory` on the buffer before it goes out of scope.

`std::memset(buf, 0, len)` may be optimized away by the compiler if `buf` is not used afterwards — that's the entire reason this helper exists.

## Dependencies

- **Internal**: none.
- **External**: platform-specific intrinsics (`SecureZeroMemory` on Windows, explicit volatile loop elsewhere).

<!-- MANUAL: -->
