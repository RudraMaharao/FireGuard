# FireGuard current circuit - Revision L

MQ-7 has been removed at the user's request. Use FireGuard-updated-circuit.png or FireGuard-updated-circuit.pdf. Earlier heater schematics and the heater/relay sections of the previous lab checklist are superseded.

## Connections

| Connection | Destination |
|---|---|
| USB-C HW-189 VCC | +5 V distribution |
| HW-189 GND | Common GND |
| CC1 | Separate 5.1 kohm resistor to GND |
| CC2 | Separate 5.1 kohm resistor to GND |
| +5 V via removable JP1 | ESP32 header marked 5V |
| +5 V | MQ-2 VCC and MQ-135 VCC |
| ESP32 3V3 | ADS1115 VDD and BME280 VCC |
| GPIO41 | ADS1115 SDA and BME280 SDA |
| GPIO42 | ADS1115 SCL and BME280 SCL |
| ADS1115 ADDR | GND (0x48) |
| MQ-2 AO | Tested 10k/10k divider + 100nF filter, then ADS A0 |
| MQ-135 AO | Tested 10k/10k divider + 100nF filter, then ADS A2 |
| BME280 CSB | 3V3 |
| BME280 SDO | GND (0x76) |
| GPIO14 | SW1 to GND |
| GPIO47 | SW2 to GND |
| GPIO21 | SW3 to GND |
| GPIO38 | 1 kohm to PN2222A base |
| PN2222A base | 100 kohm to GND |
| PN2222A emitter | GND |
| PN2222A collector | New buzzer negative |
| New buzzer positive | +5 V; use a two-pin 5 V DC active part rated at most 20 mA |
| 1N4007 for an inductive buzzer | Cathode band to +5 V; anode to buzzer negative |

All GND points are common. Camera stays connected to the FPC socket. MicroSD is disabled. MQ DO outputs, ADS A1/A3/ALRT and GPIO40 remain unconnected.

## Modules and assembly

ESP32-S3 N16R8 CAM with attached camera: 1. MQ-2: 1. MQ-135: 1. ADS1115: 1. BME280: 1. Apple USB-C charger: 1. USB-C to USB-C cable: 1. HW-189 breakout: 1. ADS 10-pin header: 1; BME 6-pin header: 1 (already soldered). Removable 5 V feed jumper: 1. Perfboard/insulated mounting and suitable wiring: one assembly/set.

## Retained small parts

- 10 kohm 1% resistors: 4
- 5.1 kohm resistors: 2
- 1 kohm resistor: 1
- 100 kohm resistor: 1
- 100 nF ceramic capacitors: 3 (two analog filters and one 5 V bypass)
- 100 uF electrolytic capacitor rated at least 10 V: 1
- PN2222A: 1
- 1N4007: 1, for an inductive buzzer with slow on/off control
- Normally open push buttons: 3
- Two-pin 5 V DC active buzzer, at most 20 mA: 1 (replacement specification; actual purchase not yet confirmed)

The prepared MQ-7 filter is spare. The LM2596, relay, second transistor and its control resistors are not used.

## Implementation and power status

The matching firmware includes ADS1115 A0/A2 readout/scaling and an explicit two-sensor relative activity model. The old CO-dependent Model A is not used. Assembled hardware, heuristic detection performance and CNN inference on this board still require validation.

User-reported bench checks: both retained filters passed, ADS1115 and BME280 each measured 3.29 V, and USB-C breakout measured 5.11 V in one orientation. The opposite plug orientation and final assembly/load checks have not been reported complete. Temporary clip connections do not validate later solder joints.

Before PC USB connection, unplug the charger and disconnect the external ESP32 5 V header feed. Never power the MQ heaters through ESP32 3V3. Final soldered wiring requires polarity and short-circuit checks.

Hybrid-location update: no new wiring. Save/verify fixed coordinates; optional Google Wi-Fi fallback and station-number SMS are configured on the local setup page. See LOCATION_SETUP.md (in docs/) for keys, defaults, freshness rules and station/test-number delivery. Current release binaries include these features.
