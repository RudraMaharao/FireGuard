# FireGuard 3 - hybrid location and optional station SMS

No wiring or hardware module is added. The circuit remains ESP32-S3 N16R8 CAM, MQ-2 and MQ-135 via ADS1115, BME280, buzzer and three buttons. No MQ-7 or dedicated CO channel.

## 1. Flash the supplied new binaries

The release/ binaries in THIS package are rebuilt with hybrid location and station SMS. Use release/README.txt or the separate firmware ZIP. Disconnect the charger/external 5 V feed before PC USB. Retain NVS for a normal upgrade; new Google and station-SMS options default off.

## 2. Saved installation pin - primary

Hold B2 for 5 s, join FireGuard-Setup and open http://192.168.4.1. In step 02, enter the building's latitude/longitude, inspect the pin in Maps, tick Use this saved location and save. A manually entered pin is only as correct as the coordinates you enter; it is not a GPS measurement. Street/flat/building/area/city/postal code, landmark and resident callback numbers should also be saved.

The saved pin works without Google geolocation. With no usable map pin, alerts still carry the written address. Save the written address even when coordinates are available.

## 3. Optional Google Wi-Fi estimate

Enable Geolocation API in your Google project, satisfy its billing requirements and restrict the API key to that API. Current Google terms/quotas apply. In local step 02 enable Wi-Fi location, enter the key, save and press Detect now. Keys are never echoed or uploaded to AWS.

The ESP32 passively scans nearby APs and sends eligible BSSIDs, signal strengths and channels to Google over HTTPS. SSIDs are not transmitted. _nomap, locally administered/multicast/reserved MACs and duplicate BSSIDs are filtered; at least two eligible APs are needed. IP-only fallback is disabled. This is not GPS and accuracy/availability varies.

A successful fix is cached in NVS and normally refreshed daily, with retries every 10 minutes when no fresh fix exists. Alert handling reads the cache; it does not wait for a lookup. New scans/requests are suppressed during alarms and an active scan is asked to stop. A network request already in progress can finish independently, without blocking alert processing.

Saved coordinates remain primary. A Wi-Fi estimate is used alone only when enabled, valid, at most 48 hours old, and reported accuracy <=2 km. A mismatch greater than max(300 m, 3 x reported accuracy) is flagged, not silently substituted. A copied estimate is still an estimate: verify it before saving as the installation pin. Disabling Wi-Fi location stops use of its fallback; a fixed saved pin continues to work.

## 4. Optional station/test-number SMS through textbee

Configure textbee in local step 06. Enter an SMS-capable full phone number in step 02, using your own test phone initially. Short service codes are rejected as SMS destinations. Enable master Forward emergency alerts and the separate Also SMS the fire-station number switch. Leave TEST MODE on for testing.

Use the explicit Send test SMS to fire-station number button. It is marked TEST ONLY. Automatic forwards are queued for either high-confidence fresh camera evidence (default flame >=0.85 and classical confidence >=0.4) or a warning unacknowledged for 60 s. Acknowledgement does not cancel a high-confidence trigger. Mute does not acknowledge. Test mode labels automatic messages TEST MODE; it still sends to the configured number.

The station message includes an address excerpt, full map link when available, postal code when supplied, callback and event identifier. Long messages can use two SMS segments. Address/landmark are shortened before map/callback/postal/event fields. Telegram retains the fuller address and available image; SMS does not attach the image. No automatic voice calls or automatic selection/contacting of a nearby station occurs.

Once-per-event means accepted by a local delivery queue, not confirmed handset receipt. Queue-full forwards can be retried. Accepted Telegram and station routes are tracked independently so one full queue does not duplicate the other. Disabling the station switch/master forwarding cancels its pending automatic sends. Destination and message are captured when queued, preventing later settings edits from rerouting an older incident. Expired jobs are dropped; ambiguous provider failures are not automatically replayed.

AWS SNS remains optional resident SMS, not the station-image route. textbee and AWS SNS may both send resident alerts if both enabled. Provider API acceptance is not proof of delivery. No station/Google credentials are included in the public source ZIP.

## 5. Tests you still perform on the board

- Verify fixed pin and written address; test with Wi-Fi geolocation off first.
- If Google is enabled, Detect now; check reported radius, source, expiry/disagreement and behavior with too few APs/API failure. No specific accuracy is promised.
- Set the station number to your own phone. Explicit test must arrive marked TEST ONLY. Check map/callback/postal code.
- Exercise unattended-warning and high-confidence policy with a test harness/simulated evidence. Do not deliberately release gas or use a real fire. Verify switch-off, acknowledgement, stale camera and queue-failure behavior.
- Confirm real hardware status/map sync to AWS and offline labeling. Confirm power stability, sample cadence and task/heap headroom during scans, inference and TLS.

References: https://developers.google.com/maps/documentation/geolocation/requests-geolocation and https://textbee.dev/docs/sending-sms/sending-sms .
