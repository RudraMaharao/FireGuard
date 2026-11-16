# FireGuard firmware - revision 2

Hardware: ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM), attached OV5640-marked camera, MQ-2/MQ-135, ADS1115 and six-pin BME280. No MQ-7, OLED, relay or SD.

| Connection | Pin / setting |
|---|---|
| I2C SDA / SCL | GPIO41 / GPIO42 |
| ADS1115 VDD / ADDR | 3V3 / GND (0x48) |
| MQ-2 divided AO | ADS A0 |
| MQ-135 divided AO | ADS A2 |
| ADS A1, A3, ALRT | Unconnected |
| BME280 VCC / CSB / SDO | 3V3 / 3V3 / GND (0x76; driver also probes 0x77) |
| BME280 GND / SDA / SCL | GND / GPIO41 / GPIO42 |
| B1 / B2 / B3 | GPIO14 / GPIO47 / GPIO21, other contact GND |
| Active buzzer driver input | GPIO38 through 1k to PN2222A base |
| MQ power | +5 V and common GND; DO unused |

See `../docs/CIRCUIT.md` and the schematic for the complete driver and power circuits.

## Build and flash

Install ESP-IDF, activate its shell and run from `firmware/`:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p YOUR_PORT flash monitor
```

The included release was built with the exact SDK version/configuration recorded in `../release/`. Dependencies are pinned in `dependencies.lock`. Flash/PSRAM and camera pin settings are in `sdkconfig.defaults` and `main/config.h`. If the serial port will not enumerate, use the board BOOT/RESET sequence and its labelled USB-OTG port for the configured native USB console.

For an upgrade, do not erase NVS or the sample partition unless deliberately clearing resident settings. Two-sensor firmware ignores old R0 values and old adaptation coefficients. Wi-Fi, Telegram and cloud settings retain their existing schema.

## Buttons

- B1 tap: mute local buzzer for up to 120 s. Hold 1.5 s: advance data label.
- B2 tap: send current readings on Telegram. Hold 5 s: open setup hotspot.
- B3 tap: save a fresh scored camera frame when one exists. Hold 1.5 s: test Telegram alert.

Camera wake-up remains driven by sensor/thermal evidence. The image view button loads the latest uploaded frame; it does not trigger the camera.

Serial commands: `m` mute; `L0`..`L9` label; `B` powered burn-in; `C` deliberately re-seed a clean-air baseline after 48 h and only while SAFE with valid channels; `p` two-sensor scores; `x` cross-validation; `s` status. `a`/`A` report that adaptive nuisance suppression is disabled for this revision. Baseline reset is not gas calibration. Start the unit in known clean air.

Revision-2 CSV has 56 positions for capture-tool stability, with removed CO positions renamed `unused_*` and unavailable quantities logged as nan. Do not use the archived three-sensor training scripts to train this hardware. Capture with `training/sensor/serial_capture.py` from this release.

Hybrid-location update: no new wiring. Save/verify fixed coordinates; optional Google Wi-Fi fallback and station-number SMS are configured on the local setup page. See LOCATION_SETUP.md (in docs/) for keys, defaults, freshness rules and station/test-number delivery. Current release binaries include these features.
