---
name: analyze-sip-core-log
description: Use when analyzing a sip_core client-side log (files like sip_core.log, *.log produced by libsip_core / sip_cli, or ui-*.log wrapping it; lines look like "HH:MM:SS.mmm  TAG  t<tid>  file:line │ message" with TAG in FTL/ERR/WRN/INF/DBG and [SIP]/[FFMPEG] payload markers). Encodes the on-disk log format and a symptom→grep playbook so you can find the fault fast. Skip for server-side Svetets SQL dumps (use analyze-sip-dump) and for raw pcap traces.
---

# Analyze a sip_core client log

`libsip_core` emits one canonical line format across the whole stack (core code,
pjsip, ffmpeg). This skill encodes it so you can go straight to the fault instead
of re-deriving the format. The authoritative source is
[docs/LOG_FORMAT.md](../../../docs/LOG_FORMAT.md) — this skill is the actionable
companion.

## Default action

1. Read this file fully.
2. Identify the log file (most-recent `*.log` the user points at, or `sip_core.log`).
3. Establish the **failure window**: `grep -nE ' FTL | ERR '` — fatals first, then
   errors. Note the `HH:MM:SS.mmm` of the first bad line.
4. Reconstruct the **timeline around it**: pull the ±N lines, and if one thread
   is implicated, filter that thread's `t<tid>` column for its full story.
5. Slice by **subsystem** using the markers (`[SIP]`, `[FFMPEG]`) and the
   `file:line` column.
6. Report: what failed, the exact timestamped line(s), the causal chain, and the
   fix or next probe. Keep it tight.

## Line anatomy (every line, no exceptions)

```
HH:MM:SS.mmm  TAG  t<tid>  file:line │ message
└─wall clock┘  │    │       └ source ┘  └ payload (may start with [SIP]/[FFMPEG])
               │    └ thread id (decimal, one call/pipeline tends to keep a tid)
               └ severity: FTL ERR WRN INF DBG   (fixed 3 chars → grep ' ERR ')
```

- Timestamp is **local wall-clock, ms precision, on every line** — bisect by time.
- No duplicate timestamps and no blank/doubled lines: pjsip's own decoration is
  stripped and trailing CR/LF trimmed. If you *do* see doubled clocks, the log is
  from a pre-`feat(logger)` build — say so.

## Severity ladder (0..5, matches the GUI slider)

| Level | Tags visible        | Meaning                                              |
|-------|---------------------|------------------------------------------------------|
| 0     | —                   | logging off                                          |
| 1     | FTL                 | fatal only                                           |
| 2     | FTL ERR             | + non-fatal errors                                   |
| 3     | FTL ERR WRN         | + warnings                                           |
| 4     | + INF               | + info, **including full raw SIP messages**          |
| 5     | + DBG               | + debug: transaction/dialog detail + 3rd-party trace |

If the user reports "I saw nothing in the log", check whether they ran below the
level a symptom needs (full SIP messages need **≥4**; codec/tsx trace needs **5**).
The knob is env `SIPLOGLEVEL=0..5` (or `LIBSIP_CORE_FLAG_DEBUG` → 5).

## Component markers

- `[SIP]` — pjsip: registration, transactions, dialogs, transport, TLS. At level
  ≥4 the payload is the **full raw SIP message** (request/response line, all
  headers, body). This is where you read the actual wire traffic.
- `[FFMPEG]` — ffmpeg + every codec behind it (x264, vpx, opus…); keeps ffmpeg's
  own `[component @ 0x..]` prefix.
- Untagged → core; locate by `file:line` (e.g. `sipaccount.cpp`, `sipcall.cpp`,
  `conference.cpp`, `media_*`).

## Symptom → grep playbook

| Symptom                        | Grep                                                       |
|--------------------------------|------------------------------------------------------------|
| Anything went wrong            | ` FTL ` then ` ERR ` (fatal first, chronological)          |
| Registration / auth fails      | `[SIP]` + `REGISTER`, `401`, `403`, `407`, `expires`, `Contact` |
| Call won't set up              | `[SIP]` + `INVITE`, `100`/`180`/`200`, `4xx`/`5xx`, `BYE`, `CANCEL` |
| SDP / codec negotiation        | `[SIP]` + `SDP`, `m=audio`, `m=video`, `a=rtpmap`, `a=crypto` |
| Read the full SIP exchange     | `[SIP]` at level ≥4 (messages are logged verbatim)         |
| Media / RTP / codec            | `[FFMPEG]`, plus `media_encoder`/`media_decoder`/`rtp` sites |
| TLS / transport                | `[SIP]` + `TLS`, `ssl`, `transport`, `handshake`           |
| Follow one call/thread         | filter the `t<tid>` column of the first bad line           |
| Registration route / failover  | `sipaccount.cpp` + `route`, `OPTIONS`, `switchToBackupRoute`, `reregister` |
| Presence stops after net change| `sippresence.cpp` / `PUBLISH` + `EUNSUP` (known: republish not re-armed) |

## Report shape

- **Verdict** — one line: what failed and where.
- **Evidence** — the timestamped line(s), verbatim, in order.
- **Chain** — how the first fault led to the symptom (use the tid timeline).
- **Fix / next probe** — the change, or the next `grep`/higher `SIPLOGLEVEL` to run.
