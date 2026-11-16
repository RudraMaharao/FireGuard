# FireGuard 3 validation - 2 November 2026

## Built and tested

Full ESP32-S3 build completed with hybrid location and station SMS using the installed ESP-IDF development checkout (reports v6.1-dev-7351-ge37a7ae137c-dirty; tooling line 6.2), Xtensa GCC 16.1.0 and pinned cached managed dependencies. No dependency source was modified. Exact config: release/sdkconfig.tested. Target: N16R8, 16 MB flash / 8 MB octal PSRAM, OV5640 support. Application: 3383840 bytes (0x33a220), 28% free in the 0x480000-byte application partition. Existing partition offsets/layout remain unchanged.

Memory fix: Wi-Fi driver AND PHY/radio plus initial hotspot now start synchronously before large worker stacks. Version 3.1 initialized only the driver early and still exhausted RAM during late PHY/timer startup. Version 3.2 also removes a further 36 KB of internal stack reservations. Large settings/pending snapshots use PSRAM-capable heap allocations; smaller radio buffers retain headroom. Flash-writing tasks keep internal stacks. A Wi-Fi init error no longer aborts local sensing. Internal/PSRAM free-heap diagnostics are logged. No settings partition is erased.

The application, bootloader and partition table in release/ are newly copied from this build. Build-info hashes identify every main source and model file. SHA256SUMS covers release files.

Host suite passed: core alert/emergency/fusion policies, camera mapping, archived sensor features, BME280 compensation/fault recovery, resident/address settings, SMS formatting/normalization/limits/request generation/secrets/disabled mode/errors, ADS mux/gain/scaling/timeout/recovery, no-CO two-sensor path, hybrid location priority/distance/mismatch/expiry, map/callback/postal/event retention in long SMS, station routing/de-duplication/full-queue retry/destination snapshots/switch cancellation, Google request construction/AP filtering/parsing/cache/NVS/failure retention/alarm pause/disabled fallback and rejection of late lookup results after settings changes.

Cloud suites passed: 7 core tests (including authenticated resolved-location sync, queued manual pin updates and refusal of local-only credential fields) plus 8 SNS tests. Tests use fake services and send no real messages. Local/cloud JavaScript syntax checked. Cloud HTML excludes Google-key and station-SMS controls, while showing synced location status/map. Circuit PDF and all four guide pages rendered and visually checked.

Run from project root:

```sh
python3 tests/run.py
python3 tests/verify_contracts.py
python3 cloud/tests/test_app.py
python3 cloud/tests/test_sms.py
```

Settings/SMS/location host tests require resolved cJSON sources (or CJSON_DIR). One inherited classical-vision unused-variable warning remains in host compilation. Host fakes do not establish timing, power integrity or radio behavior on the board.

## AWS deployment

Existing fireguard-live stack in Mumbai updated with original provisioning keys. Cloud health remains hybrid-location-v3; this device-only startup patch does not require an AWS redeploy. The URL and provisioning keys are unchanged. Deployment-verification.json in cloud/ records exact live checks, auth results, preserved resource IDs, waiting-for-hardware state and SMS disabled. No synthetic device snapshot was uploaded. No live Google lookup, SMS or Telegram message was sent.

## Physical and service checks still needed

Both prior uploads completed. The latest 3.1 boot confirms successful Wi-Fi driver initialization but ESP_ERR_NO_MEM during PHY/timer startup after worker allocation. It also records I2C probe timeouts, which remain unresolved; do not report the sensors healthy. This 3.2 replacement has passed build/host checks but needs a real-board reflash and stable boot capture. Confirm final soldering, both USB-C orientations, real I2C/camera/buzzer/buttons, sample cadence, Wi-Fi scanning/cancellation, heap/task stacks and power under concurrent CNN/TLS. Configure your own Google/textbee keys locally and verify actual receiving-handset delivery using test numbers. Provider acceptance is not proof of handset receipt.

Wi-Fi geolocation accuracy/availability is not guaranteed. Fixed pin correctness depends on installer verification. The two-sensor activity model remains heuristic, the CNN is retained unchanged, and no dedicated CO measurement or certified detection performance is claimed. See TESTING.md, LOCATION_SETUP.md and INTEGRATION_CHANGES.md.

## Reconnect verification - 2 November 2026

Rebuilt version 3.2-radio-start from the final settings.cpp correction and reran the portable host suite and source contracts successfully. Compiler stack-usage inspection no longer finds the old full-Config temporary in settings parsing. This is static frame evidence, not measured runtime stack headroom. The USB device is enumerated as /dev/cu.usbmodem101. The release and source hashes were refreshed. No new on-board flash or boot was performed by the agent; the most recent saved board capture is still the failed 3.1 boot. Use Flash-FireGuard.command from the user Terminal, since sandbox serial ioctls are blocked.

## Successful real-board startup - 2 November 2026

The user ran the Terminal flasher for the rebuilt 3.2-radio-start image (ELF prefix 39b5d9b6b). Upload hash verification passed. The subsequent 40-second capture contains one boot banner and 79 sensor rows, with the final sample at 39.279 s. No crash, reboot or I2C error was observed. The CNN model loaded and the FireGuard-Setup AP/DHCP server started at 192.168.4.1. At about 30 s, internal free heap was 60,843 bytes (largest block 31,744); PSRAM free was 6,652,664 bytes and main stack spare was 7,128 bytes. Last BME sample: 34.92 C, 48.3% RH, 1008.1 hPa. State remained WARMING.

This supersedes the earlier pending-startup status only for this short capture. An ADC sample is not proof of powered MQ heaters or gas response while external power is disconnected for PC USB. No fresh camera inference, sustained CNN/TLS stress, home-Wi-Fi connection, AWS device sync or real notification delivery was verified by the capture. Summary without credentials: hardware-boot-verification.json; raw log stays private in work/.

## Setup client detection fix - 3.3 (2 November 2026)

The user's 3.2 /api/status response reports setup=false despite the active setup hotspot. Installed SDK sources create a PF_INET6 HTTP listener when CONFIG_LWIP_IPV6=y (enabled in the tested configuration), and lwIP maps IPv4 endpoints to IPv6. The old getpeername call used sockaddr_in and interpreted the truncated IPv6 structure as IPv4.

Version 3.3 uses full sockaddr_storage for local and peer endpoints, extracts native IPv4 or IPv4-mapped IPv6, and compares the local endpoint with the actual AP address from esp_netif_get_ip_info plus the peer subnet with the actual AP netmask. Disabled AP, station endpoints, off-subnet peers, native IPv6 and malformed/truncated endpoints fail closed. CSRF and hotspot-only setting restrictions remain. No B2 wiring change or settings erase is required for this correction.

Full firmware build and portable host tests passed, including a new regression for mapped-IPv6 AP access and negative access cases. Source/sensor contracts and flasher syntax passed. Current project version: 3.3-setup-access; softwareRevision: hybrid-location-v3.3-setup. Hardware validation for 3.3 remains pending. The successful 40-second boot file records 3.2, not 3.3. AWS resources, URL and keys are unchanged.

The latest user-supplied 3.2 status also reports cameraRequested=true, cameraValid=true and visionAgeMs=423, evidence that the device reports fresh vision, but not an independent model-accuracy check. It also reports an uncalibrated GAS LEAK DETECTED state with grs about 100 and large MQ135 deviation. That heuristic state does not establish gas identity, concentration or a verified leak. Sensor power/baseline/response still need appropriate verification; do not suppress a real hazard based on software assumptions.

## Version 3.3 real-board boot - 2 November 2026

User flash hashes verified for 3.3-setup-access / ELF prefix a4c520f2d. The 40-second capture has one boot banner, 79 sensor samples, model load, hotspot/DHCP startup and no observed crash or I2C error. The final sample at 39.219 s is WARMING; BME reports 38.27 C, 41.0% RH, 1008.0 hPa. At 30 s, internal free heap is 60,791 bytes, largest block 31,744, PSRAM free 6,652,544 and main-stack spare 7,016.

This confirms 3.3 startup, not setup-client acceptance. The currently open /api/status tab still contains the earlier 3.2 response, so reconnect to FireGuard-Setup and refresh to verify softwareRevision=hybrid-location-v3.3-setup and setup=true. A fresh setup form remains unverified. Summary: docs/hardware-boot-verification-v3.3.json. Earlier generic hardware-boot-verification.json records version 3.2. No extra flash is required for these record updates.

## Current release: 3.4-save-flow (2 November 2026)

User reports that Save home settings waits indefinitely without a result. Source review found an ordering race: the primary settings POST immediately requested station Wi-Fi reconnect, while the browser still had SMS/location POSTs to perform. AP+STA share a radio; reconnection may interrupt the setup client. This is a source-supported cause, not a reproduced hardware trace of this particular failure. The old page also had unbounded fetch/body waits and no in-progress indicator.

The local page now requests deferred Wi-Fi application, completes optional settings, then calls a hotspot/CSRF-protected apply endpoint. A 2-second reply delay permits confirmation to display. A 60-second fallback applies saved network credentials if the client disappears. Only persisted SSID/password changes schedule reconnect; blank secret retention and NVS settings remain. Network scheduling is atomic and tested across clock wrap. Legacy saves without the defer header use a 2-second delay. No full Config was added to the HTTP stack.

The page shows progress and final/partial/error results next to Save, prevents duplicate submissions, bounds headers and body reads to 10 seconds, stops optional writes when primary validation fails, retains failed optional API key input, and avoids refreshing the form during a save. Successfully acknowledged secret fields clear from the browser. Unconfirmed writes are labelled unconfirmed, not failed or successful. The generated cloud page uses the same bounded save handler and does not call the local network endpoint; cloud source was rebuilt/tested but not deployed in this turn.

Validation: full firmware build; portable C++ suite including network-change detection and apply scheduling; local/cloud JavaScript save tests for ordering, validation, partial failures, lost connections, stalled body/headers and duplicate submit; cloud backend unit tests; preserved sensor/CSV contracts; static stack-frame inspection; esptool image checksum/hash. Test logs are private work/save-flow-*.log. Version 3.4 has not yet been flashed; real-board boot and Save confirmation remain pending. Existing 3.3 boot record remains historical. Use outputs/Flash-FireGuard.command with JP1 open and charger disconnected, then reconnect to FireGuard-Setup, reload the page and verify softwareRevision hybrid-location-v3.4-save. No reset/erase of settings or circuit change is required.

## Current release: 3.5-settings-storage (2 November 2026)

Real-board evidence: 3.4 flashed and booted for the saved 40-second capture, with no detected panic/reset and its expected App version. The user then supplied the new Save result: Home settings: NVS write failed. This confirms persistence failed; the 3.4 browser timeout/order fix alone did not resolve saving. Sanitized boot evidence is docs/hardware-boot-verification-v3.4.json. No numeric NVS error was available from that firmware, so partition exhaustion remains a source-supported diagnosis rather than a captured exact error code.

The legacy NVS is 24 KiB shared with settings/options/calibration. Main Config is 7952 bytes. ESP-IDF NVS writes a replacement blob before retiring the old blob, requiring spare pages and metadata. 3.5 allocates a 64 KiB data/nvs partition named fg_config at 0x800000 in previously unassigned space on the verified 16 MiB board. Existing nvs (0x9000/24KiB), phy_init, factory app and photo-storage locations and sizes are unchanged. The new partition must be flashed via the updated table; the existing launcher already writes that table. No erase_flash, formatting or deletion is performed.

Main settings prefer the new partition and copy valid legacy version-4 data if needed, retaining the original blob. Wi-Fi/bot secrets, join/hotspot PINs and registrations survive migration. Only acknowledged writes update in-memory settings; errors include esp_err_to_name and log no credentials. Initialization or migration failure preserves legacy data. SMS and geolocation writes use the larger partition too; reads fall back per missing key to the original namespace. New invalid values are not silently replaced with older values. Existing Telegram polling offsets and sensor calibration storage remain in original NVS.

Validation passed: full ESP-IDF firmware build; portable C++ suite including migration, 100 repeated saves, new-partition precedence on simulated restart, full/init failure recovery, old-blob preservation and mixed old/new optional keys; real SMS/geolocation module tests with simulated legacy write exhaustion; local/cloud save UI regression tests; preserved sensor/56-column CSV contracts; partition non-overlap/16-MiB bounds and unchanged original offsets; static stack-frame review and esptool image checksum/hash. NVS host tests are fakes for behavior, not proof of physical flash capacity, wear or power-loss recovery. Version 3.5 hardware boot and save/restart persistence remain pending.

Run outputs/Flash-FireGuard.command with JP1 open and charger disconnected; it updates app, bootloader and partition table while keeping data. Rejoin FireGuard-Setup, reload the page, verify hybrid-location-v3.5-storage and save. A second save and restart/readback are required to confirm on-board persistence. No AWS redeployment or circuit revision was performed for this device storage correction.

## Current release: 3.6-clock-init (2 November 2026)

The latest user-triggered upload log and boot capture show 3.5-settings-storage was flashed, with the expected fg_config partition at 0x800000, but booted/restarted 37 times in 40 seconds. No healthy-boot claim is made. The ELF prefix de3b0896e matches the shipped 3.5 image; resolving its backtrace with that ELF gives panic_abort -> esp_system_abort -> abort -> libc lock_init_generic (locks.c:77) -> lock acquisition -> timekeeping/gettimeofday -> time -> sensors::logRow on the first sensor tick. SDK source aborts at this branch when xQueueCreateMutex returns null during lazy libc clock lock initialization. Prior memory prints occur before the failure and do not measure heap at the failing allocation.

3.6 initializes the libc realtime lock by reading time(nullptr) before NVS initialization, Wi-Fi or any application worker starts. No clock value is set; SNTP continues to establish real network time. A failed-allocation callback uses ROM output without heap allocation and reports bytes/capability flags/caller, without credentials. This addresses the observed late initialization site; hardware validation is required to show whether it resolves all runtime memory pressure. Storage partition/migration, Save logic, sensor scoring, CNN and circuit are retained.

Validation: full ESP-IDF build; explicit source-order check that clock initialization precedes NVS, radio and sensor start; preserved sensor/56-column/partition contracts; local/cloud JavaScript save regressions; static stack-frame inspection and esptool image/hash verification. These checks do not simulate FreeRTOS internal-memory pressure. 3.6 has not yet been flashed or boot verified. The unchanged 3.5 storage/module code passed the portable host suite earlier; physical save/restart retention remains unverified in this capture.

Sanitized failure report: docs/hardware-boot-verification-v3.5.json. Raw 3.5 serial logs are private work/hardware-v35-failed-boot.log and related files; they include the setup password and must not be included in archives. All archive aliases are updated to 3.6. Use the existing outputs/Flash-FireGuard.command with JP1 OPEN and charger unplugged; keep the terminal open for its boot capture, then have the logs inspected before testing alerts. No AWS deployment or circuit change was performed.

## 3.6 board startup verification (2 November 2026)

The latest saved upload/capture matches 3.6-clock-init: upload completed, one App version startup, 79 sensor data rows ending at 39179 ms, no captured panic/reset, early clock initialization observed, model loaded and Wi-Fi obtained an address. Runtime internal free memory at 30 s was 57763 bytes (largest 29696); main stack spare 14888 bytes. Two nonfatal failed-allocation callbacks (180 bytes, caps 0x80c) appeared during startup before model load; their callers were not resolved, so memory pressure is not declared eliminated. No I2C errors were observed. Sanitized evidence is docs/hardware-boot-verification-v3.6.json.

This verifies the bounded boot capture, not long-term stability, Telegram registration/delivery, real hazard detection or saved settings after a later power cycle. Next check live Wi-Fi/Telegram/Registered fields, join the bot using the saved PIN and run an explicitly requested test alert. No further firmware change is made for this verification.

## Current release: 3.7-snapshot (2 November 2026)

User reports intermittent local/AWS offline states, SMS-button non-response and no snapshots. Direct cloud reads and Chrome confirmed cloud sync and Telegram can succeed, with Registered 1/8. The ESP32 AWS checkbox is ON, but Lambda SMS_ENABLED is separately false. The stored device/admin keys authenticate at the expected existing Lambda URL. Seven generic HTTP authentication messages occurred in a 40-second capture, but their service was not identified; do not declare the AWS key wrong. Chrome later showed textbee SMS failures; new per-service status fields aid attribution. The same capture was startup/warm-up with 78 rows and no panic or Wi-Fi disconnection, not a post-warm-up camera test.

The old camera used RGB565 double buffering and a raw DMA ring up to the configured 32 KiB; the observed largest free internal block was about 29 KiB. Capture failure due to that contiguous requirement is plausible from source/memory, but no actual camera error was captured in the short boot window. Also, old Send test alert did not wake a sleeping camera and silently skipped a missing cached photo; View image was read-only.

3.7 captures one native QVGA JPEG in PSRAM using the driver's 16 KiB JPEG DMA ring, copies the JPEG, releases the camera/DMA resources before decoding, and decodes into a 153600-byte bounded RGB565BE buffer. The original JPEG is reused for display/Telegram/cloud instead of encoding the decoded frame again. GPIO pins, model, normalization and detector RGB565BE contract are unchanged; JPEG compression may affect scores, which were not revalidated for accuracy.

Manual Capture and send photo is available locally and on AWS; cloud queues an authenticated capture command (30s expiry), and firmware requests a new frame without needing a hazard/warm-up trigger. Snapshot/test/emergency-test jobs await a fresh image after the request timestamp, bounded by a 20s fallback, and send an explicit capture-failure message if no image arrives. Automatic alert images retain their 4s freshness; cached photos may be uploaded/viewed for 120s so the 5s sync loop cannot routinely miss a one-shot image. Cloud stores image data before publishing the associated status, avoiding an old image being presented as a fresh capture.

Status includes camera ready/busy/last attempt/image/error and Telegram/cloud HTTP outcomes. Powered-monitor Wi-Fi modem sleep is disabled (increased radio power) to improve local response latency. UI controls report progress with bounded waits, preserve last readings as visibly stale on network errors, reflect alarm/fault colors, and disable AWS SMS when the deployed SNS flag is false. The cloud capture button is gated while the device still reports 3.6.

Validation: full ESP-IDF build; portable tests including camera driver/decoder fakes for one JPEG buffer, RGB565BE bounded output, release-before-decode and all failure cleanup; fresh-image wait/stale-cache/clock-wrap cases; 17 cloud backend tests including capture auth/retry/ack and no SMS side effect; JS save/gating/timeout tests; all inline script parsing; sensor/56-column CSV/partition contracts; static stack frames and esptool image/hash. These do not verify the actual OV5640, image accuracy, Telegram photo delivery or sustained connectivity. Firmware 3.7 is not flashed yet. Existing v3.6 boot evidence remains historical.

The AWS application code was updated in place and inspected in the user's Chrome tab (explicitly authorized). Lambda is Active and LastUpdateStatus Successful; deployed CodeSha256 matches work/aws-deployment/snapshot-app.zip. Only app.py and web/index.html were uploaded. Existing resources, URL, token hashes and SMS flag were retained. No live SMS, test alert, synthetic telemetry or capture command was emitted by the agent. Browser showed the new photo panel, disabled old-device capture and AWS SMS is off control.

Run outputs/Flash-FireGuard.command (JP1 OPEN, charger unplugged), let its capture finish, reload the AWS page and click Capture and send photo. That user click explicitly requests delivery to registered Telegram residents. If image capture fails, inspect cameraError and a runtime log during the manual request. No wiring change is required for this software correction.

## 3.7 real-board capture verification (2 November 2026)

The latest upload/capture matches 3.7-snapshot: one observed application start, 79 sensor data rows through 39179 ms, no panic/crash. Camera initialization at about 2.7s identified OV5640 PID 0x5640 at SCCB 0x3c, configured a 16384-byte DMA ring and a 15360-byte PSRAM frame buffer. Two nonfatal 180-byte allocation callbacks remain during startup.

Later direct status reads confirm actual 3.7 cameraReady=true, increasing cameraImageAt, empty cameraError, cloudLastStatus=200 and cloudLastError=0. Authenticated AWS /api/photo returned HTTP200 image/jpeg, 3297 bytes, valid JPEG markers, 320x240. View latest photo was clicked in Chrome (read-only; no new capture/message triggered by the agent), and the image displayed at native 320x240. The captured view is green/blurred, so normal-scene image quality is not verified. Telegram poll status was HTTP200 and Registered 1/8, but sent counter was 0 with failed requests; a fresh intentional Telegram photo delivery is still unverified. No claim of complete delivery success is made.

A separate hardware/input issue is evident: data label changes from 0 to1 shortly after startup, B3 triggers a test camera request around 2.7s, and B2 starts the setup AP around6.2s. The user confirms all buttons were untouched. This suggests button inputs are held low, possibly GPIO/GND on an internally connected terminal pair. It is not proven until isolated. User was instructed to power off and temporarily disconnect ONLY the signal wires GPIO14/B1, GPIO47/B2 and GPIO21/B3, leave camera/sensors/grounds alone, then power from PC USB with JP1 OPEN and charger unplugged. Await completion and test against a well-lit object with lens uncovered. Do not reflash just to perform this isolation.

Private raw camera image and live status remain under work/ (excluded from ZIP). A cropped user-visible screenshot of the camera panel is outputs/FireGuard-camera-live-proof.jpg outside the source archive. Sanitized evidence is docs/hardware-boot-verification-v3.7.json. Release metadata/archives now record the bounded boot and capture-to-AWS verification while retaining pending quality/delivery/button checks.
