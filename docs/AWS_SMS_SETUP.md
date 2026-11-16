# Optional SMS through AWS SNS

Lambda in ap-south-1 can send to configured resident phone numbers when ESP32 sync reports FIRE DETECTED/CRITICAL or GAS LEAK DETECTED, then an all-clear after a successfully accepted alert. MQ-7/CO is removed. No SMS for warning or suspected states. Both AWS SNS and textbee are disabled by default and independent; enabling both can duplicate alerts.

AWS SNS requires the ESP32's internet connection and successful sync, AWS SMS eligibility, and destination verification while the account is in the regional SMS sandbox. Check current sender/origination and destination-country rules in the AWS console. For India, international and registered local/DLT routes have different requirements. Configuring an Entity/Template ID alone is not a complete local-route setup. Delivery and pricing depend on the account, route and destination; no fixed demo budget or delivery guarantee is assumed.

1. Open SNS SMS in Mumbai. Check sandbox status and verify your own test destination number with its OTP. Check spend limits, origination requirements and delivery logging. Do not use an emergency-service number as a test recipient.
2. Keep the SAME deployment work directory containing provisioning.json. From the FireGuard folder:

```sh
python3 cloud/build_web.py
FIREGUARD_DEPLOY_WORK=/path/to/existing/deploy-work AWS_REGION=ap-south-1 FIREGUARD_SMS_ENABLED=true python3 cloud/deploy.py
```

PowerShell uses `$env:FIREGUARD_DEPLOY_WORK`, `$env:AWS_REGION` and `$env:FIREGUARD_SMS_ENABLED` before running `python cloud/deploy.py`. Use `false` to disable again. `FIREGUARD_SMS_ENTITY_ID` and `FIREGUARD_SMS_TEMPLATE_ID` optionally attach the corresponding registered DLT attributes; use only with the matching approved template and route configuration.
3. Flash this release, connect the device in local step 05, configure test residents and wait for sync. Open the existing AWS dashboard with the dashboard key. Device health shows AWS SMS on/off. Click AWS test SMS once, verify API acceptance, then check the receiving handset. Explicit tests are limited to one batch per 30 s.

SNS Publish permission is attached only when the stack's SmsEnabled parameter is true. Conditional DynamoDB incident claims suppress concurrent duplicate alert batches. Each hazard kind is sent once per incident, at least 60 s apart, with an overall cap including all-clear. Failed publish is reported without stopping device sync; do not treat API acceptance as confirmed handset delivery.

References:
- https://docs.aws.amazon.com/sns/latest/dg/sns-sms-sandbox.html
- https://docs.aws.amazon.com/sms-voice/latest/userguide/registrations-sms-senderid-india-routes.html
- https://docs.aws.amazon.com/sns/latest/dg/sms_publish-to-phone.html
