# SMS through textbee

This release includes textbee in the prebuilt firmware. It is off until enabled locally. No extra SMS hardware connects to the ESP32: an Android phone running textbee sends through its own SIM.

1. Create a textbee account, verify email, register the Android phone, allow SMS and keep the app online. Generate an API key. Verify a test from your textbee account first. Carrier charges and provider plan limits apply.
2. Hold B2 for 5 s. Join the setup hotspot and open 192.168.4.1. Add resident phone numbers in step 03 (10-digit Indian mobiles or +country code). Add only test recipients initially.
3. Step 06: enable SMS, enter API key, optionally select a device ID, save. Blank API key preserves a saved key. Keys remain local on the ESP32 and are not uploaded to AWS.
4. Click Send test SMS. Device health shows accepted recipient count, failures and last HTTP status. A 200/201 means API acceptance, not confirmed delivery. Confirm the actual handset received it.

Production route: ESP32 HTTPS -> `https://api.textbee.dev/api/v1/gateway/send-sms` using `x-api-key`, recipients[], message and optional deviceId. See https://textbee.dev/docs/sending-sms/sending-sms .

Events: FIRE, GAS activity, persistent-fire escalation and matching all-clear. No CO notifications; MQ-7 is removed. Warnings/readings use Telegram. Up to three hazard batches plus one all-clear per incident, hazard batches at least 60 s apart; explicit tests have a 30 s cooldown. Recipient allowance counts individually. These limits are not a guarantee against carrier/provider charges.

Pending queue is bounded; jobs expire after 10 minutes. Ambiguous HTTP/network failures are not automatically replayed because the provider might already have accepted the SMS. Inspect the provider before deliberately retesting. HTTP400/401/403/404/429 require checking request, key, device, permissions or quota in textbee's documentation; do not infer an exact cause from the status code alone.

Wi-Fi/internet and a powered, online Android phone with SMS service are required. Enabling AWS SNS too can send a second message for the same event. The local buzzer continues independently of delivery.

Optional station SMS: separate local switch in step 04 plus master emergency forwarding, valid full station/test number and configured textbee. See LOCATION_SETUP.md. Station messages may span two segments and preserve postal/map/callback/event fields. They are not SNS messages and do not attach photos.
