# sip_core log format & diagnostic playbook

This file is the contract for reading `sip_core` logs. If you (a human or Claude
Code) are handed a log file, read this first — the format is stable and
greppable by design.

## Line anatomy

Every line — from core, pjsip, or ffmpeg — has the identical prefix:

```
HH:MM:SS.mmm  TAG  t<tid>  <file>:<line> │ <message>
└─wall clock┘  │    │       └─ source site ┘  └─ payload
               │    └─ thread id (decimal)
               └─ severity: FTL ERR WRN INF DBG
```

- **Timestamp is always present** and is local wall-clock with millisecond
  precision. No line is ever undated.
- **`TAG`** is a fixed 3-char severity so `grep ' ERR '` / `' FTL '` isolate a
  level cleanly.
- **`<file>:<line>`** points straight at the emitting source (basename only).
- `│` (U+2502) is the single separator between metadata and message.

## Severity ladder (0..5) — matches the GUI slider exactly

| Level | Tag(s) shown        | Meaning                                              |
|-------|---------------------|------------------------------------------------------|
| 0     | —                   | Logging off                                          |
| 1     | FTL                 | Fatal errors only                                    |
| 2     | FTL ERR             | + non-fatal errors                                   |
| 3     | FTL ERR WRN         | + warnings                                           |
| 4     | + INF               | + info, **including full SIP messages**              |
| 5     | + DBG               | + debug: transaction/dialog detail & 3rd-party trace |

A message at severity rank R is emitted iff `level >= R`. Errors/warnings are
never silently dropped by a low debug setting — they simply need their level.

## Controlling verbosity

- Env `SIPLOGLEVEL=0..5` — the single knob; drives core **and** pjsip **and**
  ffmpeg together. Overrides the debug flag.
- `LIBSIP_CORE_FLAG_DEBUG` at `init()` → level 5; otherwise default level 4.
- Programmatic: `Logger::setLogLevel(int)` / `Logger::setDebugMode(bool)`.

## Component markers (grep anchors)

Third-party output is tagged in the payload so you can slice by subsystem:

- `[SIP]`  — pjsip (registration, transactions, dialogs, transport, TLS). At
  level ≥4 this includes the **full raw SIP messages** (headers + body).
- `[FFMPEG]` — ffmpeg and every codec that routes through it (x264, vpx, opus,
  …). Includes ffmpeg's own `[component @ 0x..]` prefix.
- Untagged payloads are core (`src/…`), located by the `<file>:<line>` column.

## Diagnostic playbook (what to grep)

| Symptom                     | Grep                                                    |
|-----------------------------|--------------------------------------------------------|
| Anything went wrong         | ` FTL ` then ` ERR ` (fatal first)                      |
| Registration / auth issues  | `[SIP]` + `REGISTER`, `401`, `403`, `Contact`, `expires`|
| Call setup / SDP            | `[SIP]` + `INVITE`, `SDP`, `m=audio`, `m=video`, `200 OK`|
| Full SIP wire trace         | `[SIP]` at level ≥4 (whole messages are logged verbatim)|
| Media / codec / RTP         | `[FFMPEG]`, plus `sipcall.cpp` / `media_*` / `rtp` sites|
| TLS / transport             | `[SIP]` + `TLS`, `ssl`, `transport`                     |
| A specific thread's timeline| filter by the `t<tid>` column                           |
| Time-window a failure       | the `HH:MM:SS.mmm` column is sortable/bisectable        |

## Guarantees (why the format is trustworthy)

1. **Timestamped**: every single line, no exceptions.
2. **No duplicates**: pjsip's own timestamp/newline is stripped
   (`pj_log_set_decor(PJ_LOG_HAS_SENDER)`); trailing CR/LF from any third-party
   payload is trimmed before write — no blank/doubled lines, no double clocks.
3. **Third-party captured**: pjsip → `pj_log_set_log_func`, ffmpeg (and all
   codecs behind it) → `av_log_set_callback`; both follow `SIPLOGLEVEL`.
