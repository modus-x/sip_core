<!-- Parent: ../AGENTS.md -->

# src/connectivity/ — Networking & string utilities

Networking helpers used everywhere — IP addressing, SIP-specific URI/header utilities, UTF-8 helpers, and the abstract socket I/O interface used by media.

## Files

| File                              | Role                                                                                              |
|-----------------------------------|---------------------------------------------------------------------------------------------------|
| `ip_utils.h/cpp`                  | `IpAddr` class (wrapping `pj_sockaddr` / `sockaddr_storage`). Host/port parse, family detection, NIC enumeration (`getAllIpInterface()`, `getAllIpInterfaceByName()`), local address query (`getLocalAddr()`). Used by `SIPAccount`, transport, ICE. |
| `sip_utils.h/cpp`                 | SIP-specific helpers: `pj_str_t` ↔ `std::string` conversions, URI parsing, header iteration, transport-type strings. `CONST_PJ_STR("...")` macro is here. |
| `utf8_utils.h/cpp`                | UTF-8 validation/transcoding (used when normalizing user-supplied display names, registered names). |
| `generic_io.h`                    | Abstract `GenericSocket<T>` template for socket-like backends (read/write/waitForData/shutdown). Used by RTP transport; `SocketPair` (in `src/media/socket_pair.h`) is the concrete RTP implementation. |

## Subdirectory

| Dir                  | What                                                       | Guide                                                                |
|----------------------|------------------------------------------------------------|----------------------------------------------------------------------|
| `security/`          | Constant-time memory ops (`memcmp`/`memset`).              | [security/AGENTS.md](security/AGENTS.md)                             |

## Gotchas

- `IpAddr` constructors accept many forms (`pj_sockaddr`, `sockaddr*`, `const std::string&`, host+port pair). Read the header before adding overloads.
- `pj_str_t` is a length-prefixed string — it is **not** null-terminated. `sip_utils::as_view(pj_str_t)` / `CONST_PJ_STR("literal")` are the safe converters.
- IP interface listing on Linux uses `getifaddrs`; on Windows uses `GetAdaptersAddresses`. Behavior on filter drivers (VPN clients) varies — log the full list when troubleshooting.

## Dependencies

- **Internal**: none beyond standard headers + pjsip types.
- **External**: PJSIP (`pjlib`), libudev on Linux (already pulled).

<!-- MANUAL: -->
