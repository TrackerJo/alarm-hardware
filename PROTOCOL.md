# Dawn clock protocol 2.0

This is the implemented protocol. It replaces the earlier JSON proposal with bounded tab-delimited commands; never send these commands to the legacy parser. All identifiers use `[A-Za-z0-9_-]+`: alarm ID <=64, owner/house/gateway <=128, request/transaction ID <=64 characters. Integers are unsigned decimal with checked overflow. All UTC times/deadlines are integer Unix seconds. Tab (`\t`) separates fields and LF (`\n`) separates participant/record lines. CR, NUL and extra fields are rejected.

## GATT

| Purpose | UUID | Properties |
| --- | --- | --- |
| v2 service | `6c120001-3c16-4d96-9abc-7b0947bda001` | Advertised |
| RX | `6c120002-3c16-4d96-9abc-7b0947bda001` | Write with response; encrypted link |
| TX | `6c120003-3c16-4d96-9abc-7b0947bda001` | Read/notify; encrypted read |
| INFO | `6c120004-3c16-4d96-9abc-7b0947bda001` | Public read, unframed hello |

With `Config::LEGACY_APP_COMPATIBILITY=true` (the default), the legacy service is advertised in the primary packet and its original characteristic executes the current Dawn app’s commands. V2 is advertised in the scan response. Legacy writes intentionally retain the original nearby-device access model; v2 HMAC does not protect that separate compatibility path. With compatibility disabled, the legacy characteristic returns `V2_REQUIRED` and does not execute commands. Link pairing uses Secure Connections/bonding with no display. HMAC with the separately provisioned key supplies application authentication; a remembered BLE address or link bond alone grants no control. Firmware generates its 32-byte device key after enabling Bluetooth RF entropy. See [Espressif's RNG requirements](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/random.html).

INFO returns:

```text
hello<TAB>2<TAB>clockID<TAB>firmwareVersion<TAB>2048<TAB>244<TAB>nonce<TAB>houseOrDash<TAB>scheduleRevision<TAB>digestOrDash<TAB>sessionID<TAB>timeValid<TAB>gatewayGeneration<TAB>droppedThrough
```

Hello is public discovery metadata, not an authenticated confirmation. Request a framed `hello` if refreshing it. The nonce changes on every connection. The application must subscribe to TX before sending commands. Requests/replies are serialized: wait for a complete application reply before the next request. Poll `event.next` to receive changes; the firmware does not push unsolicited events.

## Framing

Every RX write and TX notification has a 14-byte binary header followed by at least one payload byte:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | `0xD2` |
| 1 | 1 | flags, must be `0` |
| 2 | 4 | nonzero message ID, little endian |
| 6 | 2 | complete UTF-8 payload length, little endian |
| 8 | 2 | this fragment's byte offset, little endian |
| 10 | 4 | CRC32/IEEE of the complete payload, little endian |
| 14 | remaining | payload slice |

RX payload <=2048 bytes including authentication envelope. TX payload <=40960 bytes. Each ATT value <=`min(negotiatedMTU-3,244)`, including header. MTU 23 works with 6 payload bytes per frame. All fragments of one message carry the same ID/length/CRC and arrive in order. An identical already-received fragment is tolerated. Conflicting/out-of-order fragments, overflow, checksum failure or an inter-fragment gap over 10 seconds discard the assembly. Queue overflow disconnects the central and invalidates authentication. IDs correlate replies, not durable mutations. CRC detects damage; HMAC supplies authenticity.

A successful ATT write only queues a fragment. The signed application reply confirms the actual command outcome. A lost notification must be reconciled by reconnect/readback or retried with a new command sequence. In particular, do not infer an applied schedule from a completed BLE write.

## Authentication and framing envelopes

The key is 32 raw bytes, displayed as 64 lowercase hex characters over USB only during a physical five-second-hold pairing window. Store it privately in Keychain (phone) or server secret storage. Never put it in a public Firestore document.

The first framed request is exactly one of:

```text
claim<TAB>house<TAB>gateway<TAB>generation<TAB>signature
auth<TAB>house<TAB>gateway<TAB>generation<TAB>signature
```

`signature` is lowercase hex HMAC-SHA256 of:

```text
verb|clockID|nonce|house|gateway|generation
```

Claim requires an unbound clock and an open physical pairing window. A house cannot be changed over BLE. `auth` requires the stored house. Generation must be positive, at least the stored generation, and equal generations must name the same gateway. Higher generations atomically replace/fence the previous gateway. Firebase lease issuance and lease expiry validation are not implemented here; an authenticated holder of the house key supplies the generation.

On success, the device sends a signed `authenticated<TAB>generation`. Every subsequent request is:

```text
sequence<TAB>signature<TAB>payload
```

Its HMAC input is `C|nonce|generation|sequence|payload`. Command sequences strictly increase from 1 within the authenticated connection. Every authenticated reply is:

```text
R<TAB>sequence<TAB>signature<TAB>payload
```

Its HMAC input is `R|nonce|generation|sequence|payload`. Reply sequences have their own counter starting at 1. Clients must verify HMAC, nonce, generation and reply monotonicity before accepting a hardware confirmation. Direction markers prevent reflection. No field contains `|`; payload lines are included verbatim in the HMAC. Authentication failures before session establishment are unsigned errors and cannot be treated as confirmations.

## Schedule operations

```text
time.set<TAB>utc
schedule.read
schedule.begin<TAB>transactionID<TAB>expectedRevision<TAB>replacementRevision<TAB>count<TAB>digest<TAB>ianaZone<TAB>posixRule<TAB>house
schedule.record<TAB>transactionID<TAB>recordFields
schedule.commit<TAB>transactionID
```

UTC is accepted in 2024–2100 from the authenticated gateway. After boot, no alarm rings until `time.set` is durably accepted. It establishes a fresh scan baseline and does not catch up historical wakes. Timezone changes apply on schedule commit and cannot replay previously fired UTC instants.

Each record has 13 fields, with an optional 14th `snoozeEnabled` field (0 or 1; omitted means 1). Canonical encoding omits the field when enabled and appends TAB + `0` when disabled:

```text
alarmID<TAB>ownerID<TAB>weekdayMask<TAB>enabled<TAB>tone<TAB>recordRevision<TAB>sunMinute<TAB>monMinute<TAB>tueMinute<TAB>wedMinute<TAB>thuMinute<TAB>friMinute<TAB>satMinute
```

Sunday is bit 0; mask 0..127. Enabled is 0/1. Tone: 0 daybreak, 1 birdsong, 2 softSignal, 3 firstLight. Daily minute values are 0..1439 and apply only on enabled weekdays. Record revision is positive and <=replacementRevision. Existing IDs cannot change owner, regress revision, or change data without a newer record revision. Missing records are deleted from future scheduling; active session participants retain their original owners and tone.

Digest is eight lowercase hex digits: CRC32/IEEE of records sorted lexicographically by alarm ID, using canonical decimal fields without leading zeros, one LF **after every record including the last**. Empty schedule digest is `00000000`. HMAC authenticates the digest and all records. Golden vector: `a\talice\t6\t1\t1\t1\t420\t420\t495\t420\t420\t420\t420\n` => `012639e7`.

`expectedRevision` must equal stored revision; replacement must be strictly greater. Count <=64. All records/count/digest must match before commit. Transactions are RAM-only, canceled on disconnect, and expire after two minutes without a newly accepted record. A failed commit keeps the old snapshot. Replies: `schedule.ready<TAB>transaction`, `schedule.recorded<TAB>alarmID`, `schedule.applied<TAB>revision<TAB>digest`. Retrying the most recently committed transaction ID returns the same applied result, including after reboot. Transaction IDs must never be reused for another replacement.

Readback:

```text
schedule.snapshot<TAB>revision<TAB>digest<TAB>ianaZone<TAB>posixRule<TAB>count
recordFields
recordFields
```

The records are sorted and complete. Verify revision, digest and record data before displaying successful synchronization. Timezones require the correct IANA label **and** explicit POSIX rule supplied by the gateway; the ESP32 does not contain an IANA timezone database. For Indianapolis use `EST5EDT,M3.2.0/2,M11.1.0/2`. Update stored rules if civil-time legislation changes.

## Sessions and durable events

```text
session.get
session.stop<TAB>requestID<TAB>exactSessionID
event.next
event.ack<TAB>exactOldestEventID
sound.preview<TAB>tone
identify
```

`session.get` returns `session.snapshot<TAB>sessionID<TAB>utc<TAB>count` and participant lines. `session.stop` only succeeds against the active session or the last successfully stopped session. The request ID is validated for correlation; durable idempotency is keyed by exact session ID. A duplicate Stop returns the original durable stopped event with its original timestamp and participants. It must not trigger another Screen Time block. Preview lasts three seconds, is rejected during an active session, and is canceled if a scheduled alarm starts.

`event.next` returns the oldest unacknowledged event, or `event.none<TAB>droppedThrough`. Poll approximately once per second. Event form:

```text
event<TAB>eventID<TAB>kind<TAB>sessionID<TAB>utc<TAB>participantCount
alarmID<TAB>ownerID<TAB>localYYYYMMDD<TAB>scheduledUTC<TAB>tone<TAB>snoozeCount<TAB>deadlineUTC<TAB>originScheduleRevision
```

Kinds: `session.ringing`, `session.snoozed`, `session.stopped`, `session.expired`. Occurrence identity is `(clockID, alarmID, localYYYYMMDD)`; durable delivery identity is `(clockID,eventID)`. Session and event counters survive reboot and never wrap/reuse. Deadline 0 denotes ringing/no pending snooze. Current audible tone is the latest scheduled currently ringing participant, ties by largest alarm ID. Each event is an immutable snapshot, including owners and originating schedule revisions, so later edits/deletions do not rewrite history.

Reboot emits `session.expired` for a surviving session with UTC 0 (unknown). A session older than 24 hours also expires explicitly. No expiry is a confirmed Stop. Event storage is at most 32 entries / 40 KiB; overflow advances `droppedThrough`. Reconcile snapshots after a gap. ACK only the oldest processed event and only after durable processing; a stale or future ACK cannot delete a newer event. Disconnect before ACK is safe: the event replays with the same identity/timestamp. ACK replies are `event.acked<TAB>id`; duplicate ACKs return `error<TAB>event_not_oldest`, then `event.next` reveals progress.

Only new, verified, sufficiently recent `session.stopped` events for the relevant owner can start a local focus window. Never treat `session.expired`, a signed snapshot, an ATT response or a replay as proof of a new Stop. The future trusted backend must retain schedule revisions and derive participants from those committed records, deduplicate event identities, assign gateway leases, and deliver to the affected phones. The current backend/iOS code does not implement those paths.

Errors are `error<TAB>reason`. Retry storage failures only after resolving the fault. For a timed-out mutation, read/reconcile before creating another transaction or session action. Serial logs and the LED distinguish missing time and storage faults; a complete legacy schedule upload intentionally replaces the future schedule when compatibility is enabled.


## Current iPhone app compatibility interface (firmware 2.0.1)

Service `4fafc201-1fb5-459e-8fcc-c5c9c331914b` is in the primary advertisement, with name `Dawn`. Characteristic `beb5483e-36e1-4688-b7f5-ea07361b26a8` supports Read, Write-with-response and Notify, with a CCCD. Old commands are plain ASCII, **not** v2 frames, <=20 bytes. They are queued and executed from the main loop, never from the BLE task.

| Request | Behavior / notification |
| --- | --- |
| `BLNK` | One-second 4 kHz buzzer test on GPIO 9 (firmware 2.0.2); does not blink the LED |
| `SOND` | Three-second default melody preview, only outside an active session |
| `SETI<wallTimestamp>` | Accept the existing app’s local-wall epoch encoding and start a complete seven-slot upload |
| `SEAL<weekday><wallTimestamp>` | Sunday=0..Saturday=6; `0` disables that day; all seven slots atomically replace the recurring schedule |
| `GETE` | Enable ordered legacy `EVT:<id>:<ageSeconds>` replay |
| `ACK:<id>` | Remove only the last-delivered oldest physical event |
| `GETS` | `SNZ:<physicalPressID>:<remainingSeconds>` or `SNON` |
| `STOP:<physicalPressID>` | Exact current snooze only; durably stop the core session, then notify `STPD:<id>`; duplicate confirmed Stop returns the same confirmation |

Numbers must be decimal, positive where IDs are required, without suffixes/signs or overflow. Nonzero schedule timestamps must have the stated weekday in their wall-clock representation. Time/schedule commands accept 2024–2100. The seven-slot snapshot allows at most 10 seconds between records. An interrupted/expired upload must restart with `SETI`; its remaining fragments cannot become individual slot updates. A standalone `SEAL` is supported after complete setup and preserves other weekdays. No new application acknowledgement was invented: the existing app cannot parse one.

`SETI` can immediately synchronize an existing compatibility schedule after reboot. If replacing a v2 schedule, the UTC-to-wall-time switch is deferred until all seven slots are validated and saved. Switching time bases explicitly expires the prior active session. The stored timezone label is `Legacy/Wall`, with `UTC0` used to interpret the already-offset timestamps; firmware must not apply the house’s real UTC offset a second time. The old app supplies new wall time on sync and cannot supply future timezone/DST rule changes.

The `SNZ` identity comes from the persisted legacy ID sequence, rather than truncating/reusing a 64-bit v2 session ID. Firmware 2.0.2 reserves an ID when a scheduled session begins ringing and sends `SNZ:<id>:0` without creating an `EVT` physical press. A later ringing button press gets its own physical-event ID as before. Pressing an already silent snooze does not extend its deadline or replace its identity. Ringing snoozes repeat after 300 seconds for their first press and 60 seconds thereafter. Status repeats every two seconds while event replay is enabled; `GETS` also works independently. A disconnect preserves the active snooze mapping but cancels an incomplete schedule upload. A reboot reports `SNON` rather than fabricating a Stop for an expired session. Pending legacy event IDs come from the original `alarm-events` NVS namespace and are never reset by the migration.

This interface carries no ringtone, alarm owner or house identity. It cannot reproduce those omitted app selections. Compatibility playback uses the firmware’s configurable default tone; full participant/tone control remains on v2. A full old-app upload replaces the future v2 schedule, while retaining the house binding and all identity counters. Disable compatibility explicitly for deployments that require every control command to authenticate.

Snoozing can be disabled per schedule. The setting is captured when an occurrence starts. When any ringing occurrence has snoozing disabled, one button hit performs the normal full-session Stop, including all ringing and snoozing participants, and emits session.stopped. Otherwise hits snooze normally. Older firmware rejects the extended record, so upgrade before using this option. Storage DAWN2.2 retains this flag and can load DAWN2.1 schedules.
