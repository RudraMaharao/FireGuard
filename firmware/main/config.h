#pragma once
// One ESP32-S3 CAM. Buttons connect to GND; internal pull-ups are enabled.
#define PIN_BUTTON 14       // B1: mute / label
#define PIN_BTN_RIGHT 47    // B2: status / setup
#define PIN_BTN_WRONG 21    // B3: save latest frame / test alert
#define PIN_BUZZER 38
#define PIN_I2C_SDA 41
#define PIN_I2C_SCL 42
#define BUZZER_PASSIVE false
#define BUZZER_TONE_HZ 2700
// ---------------------------------------------------------------- camera (user ESP32-S3 N16R8 CAM, attached OV5640)
// Pin map of the common "ESP32-S3 CAM" / Freenove ESP32-S3-WROOM CAM / ESP32-S3-EYE boards.
// Check your board's pin table if the camera fails to start.
#define CAM_PIN_PWDN -1   // set this if your board wires the camera's PWDN pin (then the camera is
                          // switched fully off between scored captures; otherwise its clock is stopped)
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

#define CAM_XCLK_HZ 20000000
#define CAM_VFLIP false     // set true if the camera image is upside down
#define CAM_HMIRROR false   // set true if it is mirrored
// Frames thrown away after the camera powers up, so exposure and white balance can settle.
// More = better-lit photo but slower. 4-8 is typical.
#define CAM_WARMUP_FRAMES 6

// ---------------------------------------------------------------- detection
// A detection must score at least this to count (0.0 - 1.0).
#define FIRE_THRESHOLD 0.45f
#define SMOKE_THRESHOLD 0.45f
#define NMS_THRESHOLD 0.6f
// Class numbers and names, in the same order as data/fire.yaml used for training.
#define CLASS_FIRE 0
#define CLASS_SMOKE 1
static const char *const CLASS_NAMES[] = {"FIRE", "SMOKE"};
#define NUM_CLASSES 2


// Flash samples only: the old SD CMD GPIO38 is now the buzzer.
#define SAVE_FEEDBACK true
#define USE_SD_CARD false
#define SD_PIN_CLK 39
#define SD_PIN_CMD 38
#define SD_PIN_D0 40
#define JPEG_QUALITY 85
static_assert(!USE_SD_CARD,"SD CMD conflicts with buzzer GPIO38 in this wiring");
