"""All the knobs for the pipeline in one place.

Every script also accepts --imgsz, --epochs, --batch, --data, --device on the command line,
which override the values here for that run.
"""

# ---------------------------------------------------------------- data (shrinking step 3)
# Only the classes you need: fire and smoke. Class list lives in data/fire.yaml.
DATA = "data/fire.yaml"

# ---------------------------------------------------------------- input size (shrinking step 1)
# Lower resolution = less work: 320 needs 4x less compute than 640. On the ESP32-S3 the stock
# YOLO11n takes ~26 s per photo at 640 but ~6 s at 320 (Espressif's numbers).
# 320 also matches the camera's QVGA mode (320x240), so no detail is thrown away when resizing.
# Use 224 for ~2x more speed if your fires fill a good part of the picture; 416 or 640 if you need
# to spot small, distant flames. Must be a multiple of 32. All steps (teacher too) use this size,
# so if you change it, rerun from step 1.
IMGSZ = 320

# ---------------------------------------------------------------- step 1: teacher
# A bigger, accurate model that teaches the small one. yolo11s = 9.4M parameters.
TEACHER_WEIGHTS = "yolo11s.pt"      # COCO-pretrained, downloaded automatically
TEACHER_EPOCHS = 100

# ---------------------------------------------------------------- step 2: narrow student (shrinking step 4)
# models/fire-slim.yaml : ~60% of YOLO11n's parameters
# models/fire-pico.yaml : ~31% of YOLO11n's parameters (fastest, least accurate)
STUDENT_MODEL = "models/fire-slim.yaml"
STUDENT_EPOCHS = 200                # trained from scratch, so it needs more epochs
DISTILL_WEIGHT = 6.0                # how strongly the student copies the teacher's features

# ---------------------------------------------------------------- step 3: pruning (shrinking step 5)
# Fraction of channels removed from each prunable layer. Because a layer shrinks on both its
# input and output side, 0.3 removes roughly HALF of the parameters and compute.
# More pruning = faster and smaller but less accurate. Try 0.2 - 0.4.
PRUNE_RATIO = 0.3
PRUNE_ROUND_TO = 8                  # keep channel counts multiples of 8 (friendlier to the ESP32-S3)
FINETUNE_EPOCHS = 60                # retrain after pruning to recover accuracy

# ---------------------------------------------------------------- step 5: int8 quantization (shrinking step 2)
TARGET = "esp32s3"
CALIB_IMAGES = 256                  # images used to measure value ranges (taken from your dataset)
USE_TQT = True                      # extra accuracy tuning after quantization (slower, recommended)
TQT_STEPS = 500

# ---------------------------------------------------------------- hardware
BATCH = 16                          # lower this if your GPU runs out of memory
DEVICE = ""                         # "" = GPU if available, else CPU. Or "0", "cpu"
WORKERS = 4
