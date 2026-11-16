# FireGuard AWS deployment

The Lambda Function URL serves the dashboard and authenticated APIs. DynamoDB retains the latest device snapshot and pending commands; private S3 retains the latest available JPEG. It is not a continuous sensor-history database.

The ESP32 synchronizes every five seconds using a device key. Dashboard APIs use a separate admin key. Wi-Fi, Telegram bot token, device key and textbee keys are configured locally, not through the cloud dashboard.

## Deploy your own instance

1. Configure AWS credentials locally with an authorized account. Review `template.yaml` before provisioning resources.
2. Build the dashboard: `python3 cloud/build_web.py` from the project root.
3. Install boto3 in your local deployment environment if needed.
4. Set `FIREGUARD_DEPLOY_WORK` to a private folder outside this repository. Reuse that same folder for later redeployments; it stores provisioning keys.
5. Set `AWS_REGION` (the template defaults to ap-south-1), then run `python3 cloud/deploy.py`.
6. Enter the resulting URL and device key in the ESP32 local setup page. Use the separately generated admin key for dashboard access.

Live deployment IDs and provisioning files are intentionally excluded from the public repository. Keep generated credentials out of Git. Deploying creates AWS resources and can incur charges.

## SMS and testing

AWS SNS resident SMS is optional and disabled by default. It is separate from firmware-side textbee SMS. See `../docs/AWS_SMS_SETUP.md`. API acceptance is not proof of handset delivery. Use controlled test recipients and avoid enabling both transports without considering duplicate notifications.

Run offline tests with `python3 -m unittest discover -s cloud/tests`. Tests use local fakes and must not be represented as live device telemetry.
