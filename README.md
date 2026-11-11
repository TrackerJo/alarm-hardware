# Dawn ESP32-S3 clock firmware

## Wi-Fi alarm events (2.4.0)

Wi-Fi snooze snapshots now feed the app and the existing Live Activity push pipeline, and remote Stop requests return to the clock over the same verified connection. Bluetooth can be off. Event counters, committed participant ownership, and session/token checks protect against replays and stopping a newer alarm. See [WIFI.md](WIFI.md) for protocol and validation status. Keep OPI PSRAM enabled; the default partition layout is preserved.

## PAL connection confirmed (2.3.7)

The connected ESP32-S3 has 8 MB of embedded OPI PSRAM. Enterprise TLS exhausted internal memory while PSRAM was disabled (`mbedtls_ssl_setup`: `-0x7f00`), even after profile storage was fixed. Enable **Tools → PSRAM → OPI PSRAM** in Arduino IDE. This setting is now selected on the connected board and is the default in `tools/build.sh`. Keep the existing default partition table and flash erasing disabled.

Live verification with PSRAM enabled passed PAL certificate/name validation, Wi-Fi connection, verified HTTPS, and server acknowledgement of schedule revision 26. The phone shows Connected. Profile storage uses a bounded 1,040-byte workspace. The app recovers Wi-Fi status after Bluetooth reconnects. BLE parameter requests are deferred to avoid colliding with the phone's initial negotiation.

## Bounded Wi-Fi storage (2.3.5)

The Wi-Fi profile writer and loader now use a checked 1,040-byte workspace with aligned encrypted blocks. This removes the large contiguous-memory requirement that still failed on a live 2.3.4 attempt. Existing saved profiles remain compatible. See [WIFI.md](WIFI.md) for validation and the later confirmed PAL connection.

## Connection progress and ESP32 crypto (2.3.3)

Profile storage now uses the ESP32 one-shot AES-GCM path with two checked reusable buffers and bounded file I/O. It keeps the memory fix while avoiding reliance on the multipart implementation exercised only by host software tests. Stage-specific USB diagnostics show storage, radio/EAP initialization, authentication/IP and cloud progress. The app has a setup timeout and displays retry failures. This build has been uploaded to the connected clock with the same partition table and no data erase. See [WIFI.md](WIFI.md) for live verification.

## Wi-Fi provisioning memory fix (2.3.2)

Fixed a heap-exhaustion reboot during PAL certificate upload and encrypted profile save. The matching 2.3.1 crash ELF resolves to growing certificate strings and full-size encrypted-save/readback buffers. Uploads now use a checked bounded buffer; storage and verification stream in 512-byte blocks. Existing profiles remain compatible, and alarms/partitions are untouched. See [WIFI.md](WIFI.md) for tests and update instructions. Upload the new sketch; no erase is needed.

## Saved Wi-Fi and cloud schedules (2.3.6-wifi)

Dawn now saves PAL enterprise or home WPA2 settings through encrypted Bluetooth, reconnects after reboot/lost coverage, and downloads the complete house schedule over verified HTTPS. The saved profile is AES-GCM encrypted and separate from alarm state. Changes are confirmed only after a subsequent device readback; Bluetooth/Wi-Fi uploads cannot overwrite each other with stale replies. Setup requires no button hold. See [WIFI.md](WIFI.md) for installation, clock-time bootstrap, security boundaries and physical checks.

The sketch now includes `build_opt.h`, which disables unused C++ exception support automatically in Arduino IDE and `tools/build.sh`. Keep this file beside `alarm.ino`; do not include it as a C++ header. Normal Arduino IDE builds fit the existing app partition without custom command-line flags or partition changes. Install the updated Dawn app too. The connected clock has been updated without erasing saved data.

## Release obsolete schedule clients (2.1.1)

After a full roommate schedule has been committed (`full-` transaction), an older app's `SETI`/`SEAL` writes remain rejected and now disconnect that client. Previously the obsolete app could hold the Bluetooth connection while the updated Dawn app was still searching. Discovery resumes after disconnection; schedules, tone choices, keys and counters are retained. USB diagnostics now report connection/disconnection transitions. Close older alarm apps or disable Bluetooth on their phones during migration so they cannot repeatedly race the current gateway.

## Advertising recovery (2.0.3)

The original app service/characteristic UUIDs are unchanged. Firmware checks advertising startup and retries once per second while disconnected if the radio is temporarily busy. On ESP32-S3 it also detects advertising that stopped without a disconnect callback. The existing iPhone app does not need another rebuild for this change. USB logs now distinguish advertising active/resumed from an unsuccessful start. Stored schedules and keys are retained.

## Buzzer test and ringing notification (2.0.2)

The current app’s **Blink my clock** button now tests the buzzer: `BLNK` produces a one-second 4 kHz tone on GPIO 9, without blinking the LED or changing the schedule. The rebuilt app calls this button **Test buzzer**. Existing status LED indications remain active.

A ringing session sends `SNZ:<durableID>:0` before its first snooze press, so a connected phone can display the current alarm. This reserves an identity without fabricating a physical-button event. Normal five-minute/one-minute snoozes and confirmed Stop behavior are retained. A due time crossed while the seven-day upload is in flight is handled after commit rather than skipped.

The app’s separate missing-sync bug is also fixed in the Dawn project: saves, reconnects and foregrounding now send the schedule automatically. **Hardware alone cannot retrieve an alarm that the old app never sends.** Install the rebuilt app once for that fix. USB logging reports received legacy commands, time, record count and revision to distinguish a Bluetooth connection from a delivered schedule. No further tests were run after the user asked to stop testing.

## Existing Dawn app compatibility restored (2.0.1)

Upload the updated `alarm.ino` to the ESP32-S3. **Keep the currently installed iPhone app; no app rebuild or pairing key is required for its existing Bluetooth connection.** After uploading/resetting the clock, use Dawn's **Your clock → Find a clock** and select **Dawn** (iOS may retain the older clock name).

The old service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b` is back in the primary advertisement. Its original characteristic `beb5483e-36e1-4688-b7f5-ea07361b26a8` again accepts `SETI`, `SEAL`, `GETE`, `GETS`, `ACK:`, `STOP:`, `BLNK` and `SOND`. The v2 service remains available in the scan response; two 128-bit UUIDs are deliberately split between the two packets to fit the BLE advertising limit. Advertising resumes after disconnect.

Dawn's old wall-clock timestamps are decoded as wall time, not UTC, so alarms do not shift by the timezone offset. A `SETI` plus all seven `SEAL` slots commits as one persistent recurring schedule. An interrupted upload leaves the old schedule intact. Standalone `SEAL` updates are also accepted after a complete setup. The first snooze is five minutes and repeats are one minute. Legacy physical events retain the original NVS ID sequence, and Stop sends `STPD` only after the exact active snooze is stopped and saved. Pending events and snoozes survive a Bluetooth reconnect; a reboot still expires the session and needs time sync.

The compatibility path has the original app's nearby-device access model, without v2 key authentication. `Config::LEGACY_APP_COMPATIBILITY` defaults to `true`; set it to `false` only when using a v2 client exclusively. A complete legacy upload replaces the future schedule, including a previous v2 schedule. A switch from v2 UTC to legacy wall time explicitly expires an active session so its deadlines cannot be reinterpreted in another time basis. House binding and durable counters are retained.

The existing app sends one anonymous daily slot and **does not transmit its selected ringtone or roommate identities**. Hardware-only compatibility cannot add those missing app fields. All four buzzer melodies remain built in; compatibility playback uses `Config::LEGACY_RINGTONE` (default `0`, Daybreak). Multiple identified roommate alarms and app-selected tones still use the v2 protocol.

Blank first-use storage now initializes automatically. Existing provisioned/corrupt storage is never automatically erased. The original sketch remains in the first backup, and the pre-fix v2 code is saved in `../alarm-backups/pre-legacy-compat-20261029/`.

Dawn v2 uses your existing wiring: GPIO 18 button to ground (`INPUT_PULLUP`), GPIO 16 LED, GPIO 9 passive buzzer. No rewiring or new speaker is required. The four tones are synthesized buzzer melodies, not recordings of the iPhone previews.

The original sketch is backed up in `../alarm-backups/pre-dawn-v2-20261027/`.

## Implemented

- Up to 64 recurring alarms, stable alarm/owner IDs, per-day wake times, enable flags, four tones and schedule/record revisions. One clock binds to one house.
- Firmware-owned weekly recurrence while powered, independent of a phone connection. Normal execution catches crossed due times, including loop delays up to 24 hours.
- House IANA timezone label plus explicit POSIX DST rules. Spring gaps advance to the first valid minute; fall folds fire at the first occurrence only. Clock corrections establish a fresh baseline and do not replay historical wakes.
- Shared sessions retain all participants. Latest scheduled ringing alarm chooses the tone; equal times choose the lexicographically largest alarm ID. Every currently ringing participant snoozes independently: first press 300 seconds, subsequent presses 60. Silent snoozes keep their deadlines. Stop confirms the exact session and all affected owners, including when ringing before the first snooze.
- Atomic schedule replacement, readback/digest, persistent session/event counters, ordered event replay, exact event acknowledgements, idempotent Stop and commit retries, gateway generation fencing.
- Separate v2 BLE service with bounded framing, CRC32, application replies, link encryption and HMAC-SHA256 for every command and response. Physical pairing plus a per-clock USB-only key binds the house. BLE callbacks only queue fragments; state, flash, button and audio work run in the main loop.
- Nonblocking audio/LED updates and no startup beep. Old queued sounds cannot resume after Stop.

## Build and upload

Tested with Arduino-ESP32 **3.3.12**, board **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`), default 4 MB flash/partition settings, and **OPI PSRAM enabled** on the connected ESP32-S3 with 8 MB embedded PSRAM. All firmware dependencies ship with that board package. Keep the existing USB/flash/partition options; PAL requires the OPI PSRAM setting on this board.

```sh
./tests/run-tests.sh
./tools/build.sh
```

The build exports binaries to `output/esp32s3/`. Open `alarm.ino` in Arduino IDE, select ESP32S3 Dev Module and the actual USB port, then Upload. The connected clock has been flashed and verified with its existing partition layout. Do not erase all flash or change the partition layout of a provisioned clock: that destroys the persisted house, keys and identity counters.

Blank first-use LittleFS storage initializes automatically. A later corrupt/missing store refuses to format or reset its counters automatically. If the LED flashes rapidly, inspect the USB Serial diagnostics at 115200 baud.

## Optional v2 bench client

The current Dawn iPhone app uses the restored compatibility interface described above. The optional bench client exercises the separate authenticated v2 interface, including identified roommate alarms and tone fields. It verifies signed application replies and reads the committed schedule back. USB-assisted pairing below applies only to v2; it is not required by the current iPhone app.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/dawn_clock.py scan
```

Hold the button for five seconds while no session is active. The LED flashes quickly for a 60-second physical pairing window. Read the clock ID and 64-character pairing key in the USB Serial Monitor. Close the monitor if necessary; the key is entered privately at the tool's prompt and is never returned over BLE.

Use the peripheral address from `scan` (a UUID on macOS), and your real house ID. Replace the example alarm/owner IDs and times in `tools/example-schedule.json` before uploading to a real house.

```sh
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID --claim status
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID upload tools/example-schedule.json
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID events --ack
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID preview birdsong
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID stop SESSION_ID
```

Only use `--claim` for the initial binding. Later connections authenticate with the existing key. The tool defaults to gateway `bench`, generation `1`; use `--gateway` and `--generation` to match the current lease. A different gateway needs a strictly higher generation. Once advanced, older generations remain fenced across reboots. Lease assignment/expiry must come from the future trusted gateway service; this firmware enforces monotonic generations but does not implement Firebase leases.

## Time, storage and delivery limits

There is no battery-backed RTC. With Wi-Fi configured, the clock automatically retrieves time and schedules after reboot; see [WIFI.md](WIFI.md) for bootstrap behavior. Without Wi-Fi, **after each reboot the clock waits for time synchronization (`SETI` from the existing app or authenticated `time.set` from v2).** A surviving session emits `session.expired` with unknown time (`0`) and retained participants; it never invents deadlines or confirms a Stop. Schedule, house, counters and pending events survive. Upload synchronizes time; otherwise run:

```sh
.venv/bin/python tools/dawn_clock.py --address ADDRESS --house HOUSE_ID sync
```

For autonomous recovery during a power outage, the remaining physical addition is a battery-backed RTC, or a trusted always-available gateway that reconnects and synchronizes time. The clock reports an unsynchronized state until one of those time commands arrives.

A morning session explicitly expires after 24 hours. At most 128 occurrences are retained, covering two daily occurrences per stored alarm during a short DST day. Durable events are bounded to 32 entries and 40 KiB; overflow removes the oldest and reports `droppedThrough` in hello/`event.none`. Clients must reconcile current state after a gap. Expired, replayed or old events must never start a new app-blocking window. The last Stop result remains available separately for idempotent retries.

Snapshots are streamed to `/pending`, flushed, verified and atomically renamed to `/state`. A failed write keeps the previous in-memory state and never returns a successful durable confirmation. Runtime state and pending events commit together. Identity keys and anti-reset markers are separate NVS entries.

Legacy regression tests replay a trace produced by the existing iPhone app’s actual `LegacySchedule.commands` source. They cover discovery UUID constants, full snapshot pacing, local wall time, disabled days, per-day overrides, interrupted and failed commits, standalone slot changes, event replay/ACK order, first/repeat snoozes, stale/duplicate Stop, reconnects, reboot/time sync and v2-to-legacy migration.

LED: short pulse every two seconds = time unsynchronized; continuous rapid flash = storage/startup fault; faster flash during a pairing window; solid while ringing. A short authenticated identify blink is also available. If saving a snooze/Stop fails, the clock keeps the prior alarm state and signals storage failure.

## Verification

`tests/run-tests.sh` runs the actual C++ state machine with address/undefined-behavior sanitizers and six Python protocol/client tests. Coverage includes roommate overlap, independent snoozes, tone ordering/ties, per-day and disabled alarms, recurring weeks, DST gaps/folds, deletions during sessions, failed/interrupted commits, readback digest parity across languages, reboot expiry, durable IDs, clock corrections, stale Stop, gateway fencing, event overflow/ACK order, framing at ATT 20/244, malformed packet fuzzing, response signatures and replay rejection.

Physical checks remain: upload to your board, verify actual BLE pairing/MTU/reconnect, all four buzzer melodies, button timing, power cuts during saves, each weekday, real DST/time corrections, and retained schedules after reboot/time sync. A successful compile or desktop test does not validate physical timing, flash wear or radio delivery. Two-phone APNs/Live Activity/Screen Time fan-out and iOS v2 integration are separate app/backend work.

The wire format and integration requirements are in [PROTOCOL.md](PROTOCOL.md).
