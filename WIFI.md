# Dawn Wi-Fi connection, schedules and alarm events

Firmware **2.4.0-wifi** turns the PAL feasibility test into a saved connection. The clock remembers one network, automatically reconnects after reboot or lost coverage, and downloads the current house's complete schedule over verified HTTPS. It keeps running its last saved schedule if the internet is unavailable. A reboot still needs network time (or a Bluetooth time sync) before alarms resume; the board has no battery-backed real-time clock.

## Install and connect

1. Build and install the updated Dawn app using its existing Xcode scheme on a physical iPhone.
2. Open `/Users/nathanielkemmenash/Desktop/Hardware/alarm/alarm.ino` in Arduino IDE and keep `build_opt.h` in that same folder. This file automatically disables unused C++ exception support so normal Verify/Upload fits the existing app partition. If the sketch was already open, close and reopen it once. You can also run `./tools/build.sh` in that folder to export binaries using the same settings. No custom compiler flags or partition changes are needed.
3. Identify the actual board port with `arduino-cli board list`, then upload with:

   ```sh
   arduino-cli upload --fqbn esp32:esp32:esp32s3:PSRAM=opi --port YOUR_PORT --input-dir output/esp32s3 .
   ```

   Keep existing USB/flash/partition options and enable OPI PSRAM on the connected board. Do not erase flash or upload a filesystem image. This implementation preserves the existing partition table, schedules, clock identity, keys and event counters. Earlier verified releases were uploaded to the connected clock without a data erase.
4. Open Dawn, create or join your house, and connect to the clock over Bluetooth once.
5. Open **Your clock → Wi-Fi**. Enter your Purdue username and password. PAL3.0, the outer identity, the server name and the published certificates are configured automatically.
6. Tap **Connect**. There is no button hold or 60-second setup window. Accept iOS's Bluetooth pairing prompt if shown.
7. The page shows **Connected**, **Connecting…**, or **Not connected**, plus a short error when something needs attention. Network diagnostics and certificate settings are kept out of the form. House schedule delivery remains visible on the main clock page.


The app clears its password field when you submit. Credentials are never saved in iPhone preferences, Firestore, or logs. The clock stores them encrypted locally, independently of the alarm state file.

The Firebase endpoints and status rules are deployed in `kazoom-dawn`: `provisionClockWifi`, `revokeClockWifi`, `clockWifiSync`. The firmware's cloud host is fixed to this project. Deploying to another project requires updating the firmware host and the app Firebase configuration together.

## Daily use

Edit alarms normally in Dawn. The app saves them to the existing house collection. The clock polls about every 30 seconds; after a change it polls again promptly to confirm the actual persisted revision. Times for each day, enabled/disabled state, personal buzzer tones and snooze settings are included. Deleting the final alarm sends an explicit empty schedule.

Bluetooth remains available for nearby controls and schedule transfer. A delayed Wi-Fi response cannot overwrite a newer Bluetooth commit. An upload interrupted by a disconnect, a malformed response or a storage failure retains the old complete schedule. An in-progress Bluetooth transaction is allowed to finish before Wi-Fi installs anything.

You can view the house's Wi-Fi delivery status even when Bluetooth is disconnected. A status older than 100 seconds is shown as offline; the clock's last saved schedule still runs while powered and time-synchronized. With several clocks, this phone remembers the one it provisioned; a house with only one clock can display that clock on other members' phones too.

The Wi-Fi page contains only username, password, Connect, connection status and an error message. Re-enter credentials and tap Connect to replace the saved PAL setup. Automatic reconnection and cloud scheduling continue in the background. Existing firmware maintenance commands for pausing, reconnecting and forgetting a network remain available through the protocol, but are not exposed on this page.

Authentication failures use a longer retry delay; coverage failures use bounded exponential backoff. Network and TLS work run on a separate task so slow requests do not block buttons or buzzer timing.

This implements Wi-Fi schedule delivery. Stop/snooze controls, hardware-session publishing and Live Activity delivery retain their existing Bluetooth/phone gateway behavior; they were not moved to the clock's HTTPS channel.

## Time after a power loss

The device has no battery-backed RTC. It retains the last authenticated UTC value periodically and uses it as a lower bound during boot. After joining Wi-Fi it tries NTP to seed HTTPS certificate-date checks. Only an authenticated HTTPS response updates the alarm engine's monotonic clock. NTP never directly changes alarm deadlines.

Cold-boot enterprise authentication has a necessary bootstrap distinction: the configured CA chain and exact RADIUS name are always verified, but certificate date checks are temporarily suspended until the device obtains current time through authenticated HTTPS. Initial setup uses the time supplied by the paired iPhone. Once time is established, enterprise date checks resume on later reconnects. NTP is stopped after bootstrap; the HTTPS endpoint supplies subsequent time corrections.

This allows unattended reboot recovery on the existing board without an RTC. An external battery-backed RTC would avoid that enterprise bootstrap date exception. If campus NTP is blocked and the saved time is too old to validate the current HTTPS certificate, reconnect the app and save the Wi-Fi setup again to supply current time. A power loss with no network or phone still leaves alarms paused until time returns.

## Device access and persistence

- A separate encrypted Bluetooth characteristic, `6c120005-3c16-4d96-9abc-7b0947bda001`, handles setup. The old unencrypted alarm command characteristic cannot provision Wi-Fi.
- Each clock gets a random 256-bit cloud credential scoped to its house. Firebase stores only its SHA-256 hash. Current members can provision/revoke; outsiders cannot. A prior credential has a 24-hour rotation overlap so a failed setup does not immediately break the previous saved profile.
- The HTTPS endpoint validates that credential and current membership before returning schedules. It never accepts a house ID from a clock to select another house. Public Cloud Run invocation reaches application authentication; it does not grant schedule access.
- Wi-Fi profiles use AES-256-GCM, a random nonce, and a key derived from the clock's existing identity secret. A flushed, decrypted readback is required before atomically replacing the profile. Corrupt profiles are reported, never silently reset; the alarm state remains separate.
- The key lives in existing NVS identity storage. This protects the Wi-Fi file from casual disclosure, but is not hardware flash encryption or protection against an attacker who can extract the entire chip's storage.
- Only non-secret connection/confirmation metadata is exposed to house members in `houses/{house}/wifiClocks/{clock}`. The app compares the confirmed schedule's content digest with its current authoritative house schedule before reporting delivery.
- Network requests and responses are bounded to 40,000 bytes. Hostname and CA validation are required, with Google Trust Services R1/R4 roots bundled. Redirects and chunked responses are rejected. The live endpoint was checked against that CA bundle and its actual content-length response contract.

The original Purdue configuration provenance and public trust material are documented in `PurduePALTrust` in the app: https://cloud.securew2.com/public/62946/wl-prd-PAL3.0/android/SecureW2.cloudconfig. Nine embedded certificates were extracted and checked against the profile's fingerprints. Its additional system-store reference to InCommon RSA Server CA was not embedded; obtain an updated chain from Purdue IT if needed.

## Verification

Local tests cover native app field validation, home/PAL serialization, bounded certificate transfers, persistent status parsing, and schedule digest parity. Firmware tests exercise the actual C++ snapshot installer with stale replies, overlapping Bluetooth transactions, storage failure/retry, explicit deletion and malformed/oversized responses, alongside the existing alarm/legacy/audio/protocol suites.

Backend tests cover exact record generation, per-day times, snooze flags, empty/disabled schedules and revision handling. Isolated Firestore emulator tests cover membership, hashed credentials, delivery acknowledgement, token rotation, house binding, membership removal and revocation.

A live disposable-account check passed against the deployed backend for provisioning, auth/privacy rules, full schedule delivery, acknowledgement, edits and revocation. Its temporary accounts/documents were removed. Re-run with `node backend/scripts/check-wifi.mjs`.

Required physical checks remain: flash the board, connect to actual PAL/home Wi-Fi, confirm BLE/Wi-Fi coexistence, edit an alarm with Bluetooth turned off, verify confirmation, power-cycle and check reconnect/time recovery, interrupt a save, try a bad password, lose/recover coverage, forget the network, and verify all alarm/button/buzzer behaviors. Builds and server tests cannot validate those hardware behaviors.

## Certificate upload crash fix (2.3.2)

The 2.3.1 crash with ELF SHA prefix `225b8aef4` was decoded against the matching Arduino IDE ELF. It reaches `operator new` from certificate append and `WifiStore::save`/readback: full-size certificate, plaintext, ciphertext and verification copies exhausted the board’s heap. Bluetooth disconnects because the board aborts and reboots; the app’s former generic cancellation message concealed that cause.

The upload now uses one checked, bounded certificate buffer, with no growing string allocation per chunk. Encrypted saves, authenticated readback and boot loads stream in 512-byte blocks. Certificates are parsed individually for setup validation, and Wi-Fi buffer counts are reduced for BLE coexistence. The existing AES-GCM profile format, alarm files and partition layout are preserved. Insufficient memory reports an error instead of attempting a large throwing allocation. USB diagnostics print only certificate length and heap statistics, never credentials.

The app defers ordinary schedule/session traffic during Wi-Fi setup, retries the whole upload once after reconnecting to the same clock, and reports a real connection interruption instead of `Swift.CancellationError`. Opening the Wi-Fi page no longer starts encrypted provisioning traffic before Connect is pressed.

The native storage test uses actual Mbed TLS AES-GCM with large C++ allocations blocked. It covers a full 24 KB bundle, failed allocation, partial writes, rename failure, ciphertext/tag corruption, compatibility with 2.3.1 profiles, and forgetting the profile:

```sh
DAWN_MBEDTLS_ROOT=/path/to/built/mbedtls-3.6.x ./tests/run-wifi-store-tests.sh
```

Upload firmware 2.3.2 from Arduino IDE with `build_opt.h` beside the sketch. Do not erase flash or change partitions. Install the updated app for automatic reconnect/retry and improved error messages. Physical PAL and power-cycle verification still requires the board.

## Connection progress fix (2.3.3)

The ESP32 target uses its hardware GCM implementation, unlike the software Mbed TLS used in host tests. The profile writer now uses one-shot AES-GCM calls with two checked, reusable buffers, freed before Wi-Fi starts. No large throwing string allocations or extra readback buffers are created. File I/O remains bounded to 512-byte blocks; hardware SHA and AES operations run sequentially. Profiles remain compatible with 2.3.1/2.3.2. This supersedes the multipart crypto path described in the previous section.

Serial output now records storage encryption/write/verification/commit stages, every Wi-Fi initialization API result, radio connection attempts/disconnect reasons, IP acquisition and cloud HTTP results. It never prints login credentials or tokens. The app ends an interrupted setup after 90 seconds and shows failures during automatic retry instead of an endless Connecting label.

The 2.3.3 binary was uploaded to the connected ESP32-S3 over USB, using its existing board options with erasing disabled and a byte-identical partition table. Saved alarm/configuration sectors were not rewritten. Actual PAL connection verification is performed from the resulting Serial Monitor output and the phone.

Relevant installed SDK implementation: https://github.com/espressif/esp-idf/blob/v5.5.5/components/mbedtls/port/aes/esp_aes_gcm.c. The host storage tests cover real AES-GCM, checked first/second allocation failure, partial writes, corrupted ciphertext/tag, atomic commits, and old-format compatibility. They do not emulate the chip’s hardware crypto engine.

## Fragmented-heap profile storage fix (2.3.4)

Live 2.3.3 diagnostics showed a 31,732-byte largest free block and an 18,129-byte CA bundle. Two profile-sized allocations could not coexist even though total free memory exceeded 80 KB. The writer and loader now use a single checked allocation with in-place AES-GCM, reusing it for authenticated readback. This preserves the encrypted file format, mandatory authentication before parsing, atomic replacement, and wiping on release. The ESP32 GCM implementation permits identical input/output addresses.

The real Mbed TLS sanitizer suite verifies one allocation per save/load, full-sized certificates, compatibility with existing encrypted profiles, allocation failure, corrupted tags/ciphertext, partial writes and failed rename. Existing firmware regression tests also pass. PAL reason 23 remains an enterprise authentication failure and must be verified separately after storage succeeds; it does not identify whether credentials or server validation failed.

## Bounded profile storage (2.3.5)

A live 2.3.4 retry showed only a 17,908-byte largest block after upload, so even one profile-sized allocation was unreliable. Storage now uses one checked 1,040-byte workspace for 512-byte encrypted blocks. Encoder field boundaries are accumulated into complete AES-aligned blocks, with a partial block only at the end. The file format and key derivation remain unchanged. Boot loading first authenticates the complete file, then performs a second bounded pass to deliver settings and independently rechecks integrity. Saving verifies both the authentication tag and the original plaintext hash before atomic replacement.

Host sanitizer tests enforce the 1,040-byte allocation limit and cover 1/15/16/17/511/512/513/1,024/29,000-byte profiles, byte-at-a-time encoder fields, full CA bundles, corruption without plaintext delivery, changed encoders, allocation failures, partial writes, failed rename and compatibility with one-shot encrypted profiles. Hardware validation must confirm `Wi-Fi storage: saved`; PAL reason 23 is a separate authentication result.

## Live storage confirmation and Bluetooth status recovery (2.3.6)

The physical clock confirmed `Wi-Fi storage: verifying` followed by `Wi-Fi storage: saved` on 2.3.5. All station and enterprise setup APIs returned `ESP_OK`; PAL subsequently disconnected with reason 23. This confirms profile upload/save separately from PAL authentication. Connection and schedule delivery are not yet confirmed.

The app now re-enables Wi-Fi status notifications after reconnecting to the same clock, once the user has explicitly requested them. A successful status read clears stale Bluetooth errors. Before any explicit setup request, opening the page still does not start encrypted pairing. Authentication reason 23 is displayed without assuming a bad password. The clock requests 30–50 ms BLE intervals with a six-second supervision timeout for radio sharing, records its BLE disconnect reason, and routes SDK error output through Arduino Serial. App tests: 35 passed; signed iPhone build and installation succeeded. Default firmware: 1,298,764 / 1,310,720 bytes, unchanged partitions and no saved-data erase.

An optional `DAWN_WIFI_TLS_DIAGNOSTICS` build wraps `mbedtls_ssl_setup` and `mbedtls_ssl_handshake_step` using linker `--wrap` flags. It prints result codes, certificate verification flags and heap sizes only; it preserves return values and validation behavior. Standard Arduino IDE builds compile this diagnostic out.

## PAL TLS memory fix and physical verification (2.3.7)

The initial authentication failure was traced with a diagnostic-only linker wrapper to `mbedtls_ssl_setup` returning `-0x7f00` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`). Parsing the published 18,130-byte CA bundle succeeded, leaving 32,056 bytes total free and a 22,516-byte largest block: insufficient for the TLS buffers. The account credentials were confirmed working on another device. The connected chip reports 8 MB embedded AP_3v3 PSRAM; enabling **OPI PSRAM** makes the existing memory available without changing partitions or certificate validation. Arduino IDE has been configured accordingly, and `tools/build.sh` now defaults to `esp32:esp32:esp32s3:PSRAM=opi` (override with `DAWN_BOARD_FQBN` for an explicitly chosen board).

Enterprise identity, credentials, CA/name/date settings and method selection are applied before radio start, following Espressif's example. The standard build logs detected PSRAM/internal heap and cloud state changes without credentials. BLE connection-parameter requests wait two seconds after connection and retry up to three times after rejected/colliding updates. This avoids the HCI status 42 collision seen during the phone's initial negotiation; acceptance remains the phone's decision.

Physical results with the PSRAM build: TLS certificate verification flags zero, PAL connected on attempt 1, schedule endpoint HTTP 200, and Firestore status acknowledged revision 26 with matching desired/applied source digests. The user confirmed the phone displays Connected. Saved profiles loaded automatically after firmware restart. Temporary TLS wrappers are compiled out of the normal build. Native crypto and firmware regressions pass; the app's 35 tests pass and the signed build has been installed on the connected iPhone. Coverage-loss and physical unplug/replug testing remain separate from the verified restart path.

Final normal-firmware verification: **2.3.7-wifi**, OPI PSRAM, 1,304,822 / 1,310,720 program bytes and 50,948 bytes globals. Uploaded with unchanged partitions and erasing disabled. After restart, the saved profile automatically rejoined PAL and logged `Wi-Fi cloud: HTTP 200` followed by `Wi-Fi cloud state: confirmed; revision=26`. Final firmware regression suites pass. This result supersedes earlier sections' pending PAL authentication checks.

## Wi-Fi snooze, Live Activities and Stop (2.4.0)

Wi-Fi now carries alarm session events as well as schedules. The clock appends its bounded `clock.session` or `clock.stopped` snapshot after a blank-line separator in the existing authenticated poll. Old schedule-only clients remain compatible. A new durable engine event triggers an immediate sync; an active alarm polls every three seconds for remote Stop. Active readback timestamps refresh at most every ten seconds without incrementing the notification revision or replaying an accepted Live Activity start.

The backend resolves participants from committed schedule revisions and house membership, then uses the existing per-user snooze documents and Live Activity delivery pipeline. Both Bluetooth and Wi-Fi share the durable event sequence fence, so an older transport cannot resurrect a stopped session. Wi-Fi never takes ownership of the Bluetooth gateway. Device credentials and provisioning-member membership are rechecked in the session transaction, including credential rotation/revocation.

The response adds `wifi.control\tSEQUENCE\tCORE_SESSION\tPUBLIC_TOKEN` after a blank line. Zero identity means no Stop. A nonzero Stop is applied on the alarm loop only when the response sequence and the clock's durable session/token mapping match; existing engine persistence and idempotency remain in force. The app keeps Stop pending until the physical clock reports completion. Expired retained Stop snapshots become idle readback, without crediting a new personal wake. An event whose wall time has drifted can retry after the schedule response corrects network time.

Verified before hardware upload: firmware sanitizer/regression suites, 11 backend unit tests, Firestore emulator coverage for Wi-Fi snooze, ownership spoof rejection, duplicate-event heartbeat, remote Stop, stopped-session replay protection, idle cleanup and time recovery; and a disposable-account test against the deployed backend covering live event publication and remote Stop acknowledgement. Temporary live test data was removed. All 39 app tests pass. The normal firmware build uses 1,306,198 / 1,310,720 bytes with OPI PSRAM and the unchanged partition table. Physical Wi-Fi-only snooze/Live Activity verification is pending the clock upload and a real button press.
