<!-- Parent: ../AGENTS.md -->

# src/im/ — Instant messaging (SIP MESSAGE)

Send/receive SIP MESSAGE (RFC 3428) inside or outside a call. Two layers: **`instant_messaging.cpp`** (per-INVITE messages, multipart/mixed) and **`message_engine.cpp`** (account-level outbox with retry, status tracking, persistence).

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `instant_messaging.h/cpp`           | Build PJSIP MESSAGE bodies (single-part or `multipart/mixed`). Parse incoming MESSAGE bodies into `{mimeType: payload}` maps. Sender helpers used by `SIPCall::sendTextMessage` / `SIPAccount::sendMessage`. |
| `message_engine.h/cpp`              | `MessageEngine` — owned by `SIPAccountBase`. Account-level outbox: queues messages, retries on connectivity, persists to disk so re-deliveries survive restarts. Tracks `MessageStatus { UNKNOWN, IDLE, SENDING, SENT, DISPLAYED, FAILURE, CANCELLED }`. |

## Public API → engine flow

```
sendAccountTextMessage(acc, to, {mime → payload})       [config interface]
  -> Manager::sendTextMessage
  -> Account::sendTextMessage
  -> SIPAccount::sendMessage
       -> MessageEngine::sendMessage (returns MessageToken)
            -> instant_messaging:: build MESSAGE
            -> PJSIP transmission
  -> getMessageStatus(token) lets the host poll
```

Incoming MESSAGE flow:

```
PJSIP MESSAGE module callback (SIPVoIPLink registers it)
  -> SIPAccountBase::onTextMessage
  -> instant_messaging:: parse body (via handleMessage)
  -> emitSignal<ConfigurationSignal::IncomingAccountMessage>
     (in-call text messages are delivered via emitSignal<CallSignal::IncomingMessage>
      dispatched from Manager::incomingMessage in manager.cpp)
```

## Read receipts

The IMDN (RFC 5438) workflow is supported but optional. `setMessageDisplayed(account, conversation, msgId, status)` sends a Message Disposition Notification back; `AccountConfig::sendReadReceipt` toggles whether incoming messages are acknowledged.

## Gotchas

- Body MIME types: `text/plain` is the default. `application/im-iscomposing+xml` is used for typing indicators. Custom MIME types are passed through unchanged.
- The persistent outbox file is under `data_path`; if you change its format add migration code.
- `cancelMessage(account, id)` only works while the message is still `IDLE` or `SENDING` — once `SENT` it's gone.

## Dependencies

- **Internal**: `sip/sipaccount.h` (for transmission), `client/ring_signal.h` (for incoming-event dispatch).
- **External**: PJSIP for MESSAGE method support.

<!-- MANUAL: -->
