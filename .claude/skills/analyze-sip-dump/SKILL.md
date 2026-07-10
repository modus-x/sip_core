---
name: analyze-sip-dump
description: Use when analyzing SQL log dumps from the custom Svetets / s112 SIP-server-proxy (files named like dumps/logs_dump_<TS>.sql or similar MySQL dumps with objects_alive_*, objects_died_*, events_* tables). Produces a structured report (default Russian) identifying participants, timeline, who initiated disconnect, and notable SDP/RTP/SIP anomalies. Skip for generic SIP traces (pcap, syslog, asterisk full) — this skill is specific to the Svetets CallControl dump format.
---

# Analyze a Svetets / s112 SIP-server log dump

The user has a custom SIP-server-proxy + registrar (identifies as `Svetets CallManager 9.2-276`). Its debug subsystem exports state to MySQL dumps with three tables per snapshot: `objects_alive_<ts>`, `objects_died_<ts>`, `events_<ts>`. This skill encodes the format so you don't have to re-derive it every time.

## Default action

1. Read this file fully.
2. Confirm the target dump path with the user only if ambiguous; otherwise use the most-recent `dumps/logs_dump_*.sql` in the cwd.
3. Run the **Fast triage** queries (§4) to get participants, timeline anchors, and the disconnect initiator.
4. Drill into specifics only if the user's question requires it.
5. Write the report to `dumps/<dump_basename>_report.md` using the template in §7. **Russian by default** — only switch if the user asks in English or another language.

Keep narration short. The user asked for "as quick as possible" — don't re-derive what's already in this file.

---

## 1. File format

A MySQL dump (`.sql`) producing three tables. Suffix `_<TS>` is the dump timestamp (e.g. `_20260520170757`), **not** the call time.

| Table | Contents |
|-------|----------|
| `objects_alive_<TS>` | Objects still active at dump time. Often empty if the call already ended. |
| `objects_died_<TS>` | All objects that ended before the dump: CC-sessions, SIP-Connections, fetches, scenario references. Each row has `id`, `s_id` (parent), `display_name`, `object_type`, `birth_date`. |
| `events_<TS>` | The event stream. Columns: `id`, `log_type`, `log_text`, `log_label`, `event_source`, `event_target`, `event_id` (PK, monotonic), `event_time`. |

**Critical ordering rule.** Rows appear in the file in **reverse chronological order** (newest first). To reconstruct the call, sort by `event_id` **ascending**, not by file line number. The `event_time` field has microsecond precision and is reliable.

Encoding: UTF-8 with mixed CRLF/LF and very long lines — pipe through `cat -A` only if you need to spot bare CRs. Russian text is common in `display_name` and JS Dumps.

---

## 2. Identifiers and conventions

Identifier shapes you will see, and what they mean:

- `<sessionPrefix>-<random>` — CC-Connection (one SIP leg's logical wrapper). Example: `6693-70a6`.
- `cn<connectionId>` — SIP-Connection (the actual SIP dialog under the CC-Connection). Example: `cn6693-70a6`. Use this to grep raw SIP traffic.
- `<sessionPrefix>-<longRandom>` — CallControl session (B2BUA-level session glue). Example: `6693-NSVuYyE5cFAL`.
- `prst-<random>` — **Presence / ACD-state session** for an operator. **Not a SIP leg.** Tracks operator status (`registercall`, `goodoperator`, `unregistercall`). Ignore when listing call participants.
- `<random>@<ip>` form for CC-Connections (e.g. `6693-QdoHaU0MT2JV@10.6.5.16`) — the `@<ip>` part is just part of the ID, not a SIP URI.

Recorder filenames embed semantics: `session_<ccSessionId>_<userId>_L.rsf` (operator side audio), `_R.rsf` (remote side), no suffix = mix.

Common `log_type` values: `Event`, `JS Log`, `JS Dump`, `Trace`, `SIP Message`, `RTP Operation`, `RTP Event`, `Telephony Event`, `Telephony Operation`, `Fetch Request`, `Fetch Response`, `HTTP Message`, `Info`, `State`, `CCConnection`, `Redis`, `Cached Doc Provider`.

---

## 3. Field semantics — the gotchas

These are the field traps that cost time. Memorize:

### 3.1 `event_source` / `event_target` on SIP/HTTP/RTP rows

Direction of the packet on the wire:
- `event_source=CALLMANAGER, event_target=6693-70a6` → server **sent** the packet to the leg.
- `event_source=6693-70a6, event_target=CALLMANAGER` → server **received** the packet from the leg.

Don't confuse with the `From:` / `To:` SIP headers — those are dialog-fixed for the lifetime of the call regardless of which side sent any given message.

### 3.2 `originator` vs `initiator` (both appear in `connection.disconnected` JS Dump)

The single biggest source of confusion in this format:

| Field | Meaning | Values |
|-------|---------|--------|
| `originator` | Who **placed** the call (whose INVITE was first). | `local` = server placed call (outbound leg), `remote` = SIP peer placed call (inbound leg). Frozen for the dialog's lifetime. |
| `initiator` | Who **hung up** (whose BYE was first). | `local` = server sent BYE, `remote` = SIP peer sent BYE. Set only at disconnect. |

When answering "who hung up?", read `initiator`, **not** `originator`. A leg can have `originator=local, initiator=remote` (server called out, peer hung up) or vice versa.

### 3.3 Cascade pattern

In a B2BUA bridge, when one leg hangs up, the server tears down the other. So you typically see:
- Leg A: `initiator=remote` (peer hung up first) — this is the **true** initiator.
- Leg B: `initiator=local` (server hung up as cascade) — this is the **follow-on**, not the cause.

The leg with the **earliest** BYE `event_id` is the real cause. The other side's `initiator=local` is just the cascade.

### 3.4 "conference.joined" ≠ multi-party conference

The `conference.joined` Telephony Event you'll see on every 2-party call is the **internal RTP-join** of the two B2BUA legs into a bridge. It is **not** evidence of a 3+ party conference. Look for additional Call-IDs or `dvo_conf_connect` DTMF events for that.

### 3.5 `application/confInfo+json` INFO messages

These are sip_core's **conference metadata** (see `src/call.cpp:788` `Call::sendConfInfo`, built with `CONFERENCE_METADATA=ON`). Body looks like `{"h":0,"layout":0,"v":1,"w":0}`. Their presence means the operator's softphone has the Conference feature active, but doesn't by itself prove multiple SIP legs in *this* dump — other legs may be in other sessions/servers.

### 3.6 DVO feature codes

Domain fields like `dvo_conf_connect: "#3#"`, `dvo_conf_disconnect: "#4#"`, `dvo_hold: "#5#"`, `dvo_transfer: "*"`, `dvo_fax: "#2#"` are **DTMF mid-call feature codes** (DVO = «Дополнительные виды обслуживания», Russian PBX supplementary services). They're just configuration — to know if they were *used*, look for DTMF / RFC 2833 events on the RTP side or `telephone-event` `payload 101` packets. If there are no DTMF events, the codes weren't dialed.

### 3.7 Scenario names hint at call type

`scenario: "s112_conf.js"` → emergency-112 dispatch with conference capability. Other names: plain ACD, IVR, FMC. Find via `grep "scenario:"`.

---

## 4. Fast triage queries

Run these first. Replace `$F` with the dump path. They each finish in milliseconds.

```bash
F=dumps/logs_dump_<TS>.sql

# 1. All dialogs (SIP Call-IDs). Two = simple 2-party call. More = multi-leg.
grep -oE 'Call-ID: [^[:space:]]+' "$F" | sort -u

# 2. All SIP URIs that participated.
grep -oE 'sip:[A-Za-z0-9_+.-]+@[A-Za-z0-9_.-]+' "$F" | sort -u

# 3. All CC objects (sessions + connections + fetches) with birth time.
grep -A 20 'INSERT INTO objects_died' "$F" | grep -oE "'[^']+','[^']*','[^']*','[^']*','[^']*'" | head -30

# 4. All BYE / CANCEL events with line numbers — find termination signals.
grep -nE "',(BYE|CANCEL)," "$F"
# (the explicit comma anchors the log_label column — avoids matching 'Allow: BYE' headers)

# 5. Who initiated the disconnect — read 'initiator' on each leg.
grep -oE '"connectionid":"[^"]+", "connectionduration":[0-9]+, "initiator":"[^"]+"' "$F"

# 6. Earliest BYE wins. Get BYE rows with event_id and event_time.
grep -B1 -A2 "'BYE'," "$F" | grep -oE "'[0-9]{8,}','2026-[0-9-]+ [0-9:.]+'"

# 7. Scenario / call type.
grep -oE 'scenario: *"[^"]+"' "$F" | sort -u

# 8. User-Agent (identifies the operator's softphone — usually 'Svetets Svetophone X.Y.Z').
grep -oE 'User-Agent: [^\r\n]+' "$F" | sort -u

# 9. Re-INVITEs (mid-call SDP renegotiation). Filter out 'Re-INVITE' literal trace strings.
grep -nE "',[A-Z]+ \(SDP\)?'," "$F" | head

# 10. State transitions per connection (call lifecycle).
grep -E "',State'," "$F"

# 11. Conference metadata (sip_core's Conference feature in use).
grep -nE 'application/confInfo\+json|confOrder|confVoiceActivity' "$F"

# 12. DTMF events actually fired (not just configured).
grep -nE 'telephone-event|dtmf|DTMF' "$F" | grep -v 'rtpmap\|fmtp\|RTP/AVP'
```

Process the file with **sort by `event_id` ascending** when you need a real timeline. Quick command to extract `(event_id, event_time, log_type, snippet)` chronologically:

```bash
grep -oE "'[A-Za-z0-9_@.-]+','[A-Za-z][^']*','[^']{0,80}'[^,]*,'[^']*','[^']*','[^']*','[0-9]+','[0-9-]+ [0-9:.]+'" "$F" \
  | sort -t"'" -k14 -n
```

(That regex is approximate — for serious timeline work prefer loading into sqlite as in §6.)

---

## 5. Disconnect-initiator decision rule

Apply in this order — first match wins:

1. **Compare BYE timestamps.** Find every `BYE` SIP Message row. The one with the **smallest `event_id`** (or earliest `event_time`) is the true initiator. The other side's BYE is the cascade.
2. **Verify with `initiator` field.** On the leg whose BYE came first, `connection.disconnected.initiator` should be `remote` if the SIP peer on that leg sent the BYE, `local` if the server sent the BYE.
3. **Verify with packet direction.** On that BYE row, check `event_source` vs `event_target`:
   - `event_source=<connId>, event_target=CALLMANAGER` → **peer sent BYE** (peer initiated).
   - `event_source=CALLMANAGER, event_target=<connId>` → **server sent BYE** (server initiated; very rare as a true cause — usually a downstream cascade or a timer).
4. **Look for the trigger.** If the initiator was a SIP peer (softphone), scan the last ~5 seconds before the BYE for: failed re-INVITE, malformed SDP answer (missing `m=video` line when offered — RFC 3264 §6 violation, the server here does this), 4xx/5xx on INFO, RTP timeouts, DTMF feature codes. Note these as **possible** triggers but don't claim them as causes unless there's a direct error log. A 4–6-second gap between a SIP anomaly and a BYE is consistent with **manual** hangup by the operator after they noticed the issue; sub-second gaps suggest automatic action by the client stack.

Common false leads:
- `500 Connections Unjoined` and `500 Internal Server Error / Explicit dialog termination` responses to INFO messages near the disconnect → these are **late replies to in-flight INFOs**, not the cause. Check their CSeq to see they're for an earlier request.
- Repeated `200 OK` retransmits from an SBC (e.g. Eltex SBC) → packet loss on that side, can be a *symptom* but rarely the direct cause of the disconnect unless paired with an actual SIP timer firing.

---

## 6. Optional: load into sqlite for ad-hoc queries

For dumps larger than ~5MB or when the user wants arbitrary queries, transform once:

```bash
F=dumps/logs_dump_<TS>.sql
DB=/tmp/dump.sqlite

# Strip MySQL-isms and run through sqlite. The dump uses `;;;` as statement separator
# and ENGINE=MyISAM clauses sqlite rejects. Crude but adequate transform:
python3 - <<PY
import re, sqlite3, sys
src = open("$F", encoding="utf-8", errors="replace").read()
src = re.sub(r"ENGINE=MyISAM[^;]*", "", src)
src = re.sub(r"collate utf8mb4_bin", "", src, flags=re.I)
src = src.replace(";;;", ";")
src = re.sub(r"datetime\(6\)", "TEXT", src)
src = re.sub(r"varchar\(\d+\)", "TEXT", src)
src = re.sub(r"int\(\d+\)", "INTEGER", src)
src = re.sub(r"KEY[^,)]*[,)]", ")", src)
src = re.sub(r"PRIMARY KEY[^)]*\)", "", src)
src = re.sub(r"AUTO_INCREMENT=\d+", "", src)
con = sqlite3.connect("$DB")
for stmt in src.split(";"):
    s = stmt.strip()
    if not s: continue
    try: con.executescript(s + ";")
    except Exception as e: print("skip:", str(e)[:80], file=sys.stderr)
con.commit()
PY

# Then list tables and query chronologically:
sqlite3 "$DB" "SELECT name FROM sqlite_master WHERE type='table';"
sqlite3 -header -column "$DB" \
  "SELECT event_id, event_time, log_type, event_source, event_target, substr(log_text,1,60) AS snippet
   FROM events_<TS> ORDER BY event_id ASC LIMIT 50;"
```

Don't reach for sqlite unless the user asks for ad-hoc queries or grep is getting unwieldy. The fast-triage queries in §4 answer 90% of cases without it.

---

## 7. Report template

Write to `dumps/<source_basename>_report.md`. **Russian by default.**

```markdown
# Анализ дампа `<filename>`

## Краткое резюме

**Кто оборвал?** <одной строкой: сторона + URI + ip:port>, отправив SIP `BYE` в `<HH:MM:SS.mmm>`.
Сервер каскадно завершил вторую ногу.

**Оговорки.** <если в дампе одна сессия, а пользователь говорит про N-стороннюю конференцию —
напомнить, что остальные ноги могут быть в других сессиях / локально в sip_core>.

## 1. Формат лога
<1-2 абзаца: что такое objects_alive / objects_died / events; правило reverse-chronological;
смысл originator vs initiator; что cn-префикс = SIP-Connection; что prst- = presence>

## 2. Участники

### 2.1. <inbound leg> — `<conn_id>`
| | |
|--|--|
| Call-ID | |
| originator | remote / local |
| Удалённый URI | |
| Локальный URI | |
| user_id | |
| Длительность | <sec> |
| User-Agent | |
| Запрошенные медиа | <m= lines> |

### 2.2. <outbound leg> — `<conn_id>`
<то же>

### 2.3. Presence/ACD (если есть) — `prst-...`
<одной строкой: не SIP-нога, нужна для статуса оператора>

### 2.4. Где могут быть «другие участники» <если применимо>
<гипотезы: локальная конференция sip_core; другая CC-сессия; другая АТС>

## 3. Хронология
<разбить на смысловые этапы: установление вызова → очередь/маршрутизация → мост →
mid-call события (re-INVITE / INFO / hold / transfer) → разрыв>

Каждая строка: `HH:MM:SS.mmm` | событие (с указанием SIP CSeq, кода, направления).

## 4. Кто оборвал и почему

### 4.1. Кто (факт)
<3-4 независимых индикатора: первый BYE по event_id; initiator field; направление пакета;
каскад>

### 4.2. Почему (гипотезы)
<список нетипичных событий за ~5 сек до BYE: SDP issues, потерянные ACK,
отвергнутые INFO, DTMF, RTP-таймауты. Маркировать «возможный триггер», не «причина»,
если нет явного error log>

### 4.3. Контр-аргументы к альтернативным версиям
<«сервер оборвал» / «удалённый оборвал» — почему эти версии не подтверждаются>

## 5. Итог
<3-5 буллетов: бесспорные факты + явные оговорки>

## 6. Что стоит проверить дополнительно
<bullet list: клиентский лог sip_core, pcap, другие сессии на том же сервере,
конфигурация SDP-обработки на сервере>
```

When the user asks specific questions ("who hung up?", "was there a third party?"), answer **first** in 1–2 sentences in chat, **then** offer the full report. Don't write the full report unless they ask or it's clearly the original ask.

---

## 8. Things that look broken but aren't

- **Server `200 OK` to a video-offering re-INVITE has no `m=video` line at all.** This violates RFC 3264 §6 (rejected media should be `m=video 0 ...`), but it's how this server has behaved historically. Note it, don't treat it as a bug-of-the-day.
- **`P-Eltex-Diagnostic: Explicit dialog termination`** on a `500` from an Eltex SBC — this is the SBC's way of saying "the dialog was being torn down when this request arrived." Not a server bug.
- **`500 Connections Unjoined`** on an INFO with `application/confInfo+json` arriving right around a BYE — late reply to an INFO that crossed the BYE in flight. Effect, not cause.
- **`File created successfully` / `JS Log: !!!!!!!!! s112 session dead`** — debug-shouting in the JS scenario, normal end-of-session logging in `s112_conf.js`. Not an error.
- **Multiple `200 OK` retransmits with the same CSeq from a far-side SBC** — RFC 3261 reliable response retransmission. Indicates packet loss of the ACK on that hop, not a bug in the server.

---

## 9. When to escalate / ask the user

- The dump has **only one SIP leg** but the user describes a **multi-party** conference → ask whether they have the other server-side dumps, or whether the conference was local in sip_core (Conference class, see `src/AGENTS.md` → "Conference handles multi-party calls").
- The dump has **no BYE at all** → the call may still be active; check `objects_alive_*`. If empty too, the dump may be truncated.
- The disconnect happened on the **first** ring before INVITE (CANCEL flow rather than BYE) → handle separately; CANCEL semantics are different (cancels an in-progress INVITE).
- The user asks "why did sip_core send BYE?" → state plainly that **this dump alone can't answer that** — you need the client-side sip_core log (look under sip_core's logger output). Recommend they run with `SIP_CORE_DBG` enabled and reproduce.

---

## 10. Output checklist before finishing

- [ ] Report written to `dumps/<basename>_report.md`.
- [ ] Russian unless user asked otherwise.
- [ ] First sentence states *who* initiated the disconnect.
- [ ] Each timeline entry has microsecond timestamp + CSeq where applicable.
- [ ] `originator` vs `initiator` correctly distinguished (do not conflate).
- [ ] If the user mentioned a participant count that doesn't match the dump, the discrepancy is acknowledged with concrete reasoning.
- [ ] Suspect triggers are labeled "possible", not "cause", unless a direct error log proves causation.
