# Current hardware notice

The current two-sensor sensor path is documented in sensor/README_TWO_SENSOR.md. The legacy three-sensor sensor trainer is archived and disabled. The camera training workflow below remains available; the CNN was not retrained for this release.

# Fire detection: shrinking YOLO11 for the ESP32-S3 CAM

This project trains a fire/smoke detector based on YOLO11. It then shrinks the model with five techniques and turns it into a `.espdl` file that Espressif's **ESP-DL** library runs on the ESP32-S3.

| Shrinking technique | Where it happens |
|---|---|
| Lower input resolution (320×320) | `IMGSZ` in `settings.py`, used by every step |
| Only the classes you need (fire, smoke) | `data/fire.yaml` |
| Narrower network + knowledge distillation | `models/fire-slim.yaml`, steps 1–2 |
| Structured pruning + distillation fine-tune | step 3 |
| int8 quantization | step 5 (Espressif's ESP-PPQ) |

## How much smaller

Measured with 2 classes:

| Model | Parameters | GFLOPs at 320 | Rough ESP32-S3 time at **320** (default) | GFLOPs at 224 | at 224 |
|---|---|---|---|---|---|
| YOLO11n (stock) | 2.59 M | 1.61 | ~6.2 s (Espressif's figure) | 0.79 | ~3.2 s |
| fire-slim | 1.56 M | 1.05 | ~4.2 s | 0.51 | ~2.1 s |
| **fire-slim + prune 0.3** (default) | **0.77 M** | **0.48** | **~1.9 s** | 0.24 | ~0.9 s |
| fire-pico | 0.81 M | 0.60 | ~2.4 s | 0.29 | ~1.2 s |
| fire-pico + prune 0.3 | 0.35 M | 0.25 | ~1.0 s | 0.12 | ~0.5 s |

For comparison, stock YOLO11n at 640 takes about 26 s per photo (Espressif's figure).

The times are scaled from Espressif's YOLO11n benchmark by compute. Treat them as rough guides; real timing only comes from running it on the board. After int8, the model file is roughly 1 byte per parameter (the default comes out a little under 1 MB).

**Image size** (`IMGSZ` in `settings.py`) defaults to 320, which matches the camera's 320×240 QVGA mode.
- Use 224 for about 2× more speed, if the fire usually fills a good part of the picture.
- Go back up to 416 or 640 only if you need to catch small, distant flames.
- Every step, including the teacher, uses this size, so rerun from step 1 after changing it.

## Setup

A **GPU is strongly recommended** for training; on a CPU it takes days.

**No GPU? Use `train_on_colab.ipynb` (free Google Colab T4 GPU).** Upload this folder and your dataset zip to Google Drive, open the notebook in Colab, and run the cells top to bottom. It does steps 0–5 for you, saves progress to Drive, and downloads the finished `.espdl` at the end.

On your own PC with an NVIDIA GPU, use **Python 3.12**. Espressif's quantizer (steps 4–5) doesn't support 3.13 yet:
```
pip install -r requirements.txt -r requirements-export.txt
```
On Python 3.13, `requirements.txt` alone is enough for training (steps 0–3, 6–7). Run steps 4–5 in a separate Python 3.12 environment; the Colab notebook does this for you.

## Dataset

1. Get a fire/smoke dataset with **bounding boxes**. Roboflow Universe has many: search "fire smoke detection", pick one with a few thousand images, and download it as **YOLOv11** (zip).
2. Run `python 0_prepare_dataset.py path/to/the.zip`. You can pass several datasets, and add photos **without** fire using `--negatives`:
   `python 0_prepare_dataset.py rf1.zip rf2.zip --negatives Fire-Smoke-Dataset.zip my_room_photos/`
   For DeepQuestAI's Fire-Smoke-Dataset, only its "Neutral" folder is used, because its fire/smoke photos have no boxes. The script:
   - reads the dataset's class names and maps them to **0 = fire, 1 = smoke**, the order the ESP32 firmware expects, whatever the download calls them;
   - drops other classes and converts polygon labels to boxes;
   - builds `datasets/fire` with a train/val split;
   - writes `data/fire.yaml`.
3. Add "tricky" images with no fire via `--negatives`: sunsets, orange lamps, red clothing, candles on a phone screen, and so on. They stop the model from learning that "orange means fire".
4. A few hundred photos taken with **your own ESP32-S3 camera** make a big difference, both for training and for calibration in step 5.

## Run it

```
python 0_prepare_dataset.py data.zip   # dataset -> datasets/fire, classes 0=fire 1=smoke
python 1_train_teacher.py      # big yolo11s teacher          (~1-2 h on a T4 for a few thousand images)
python 2_train_student.py      # narrow student, distilled    (the longest step: trained from scratch)
python 3_prune.py              # prune channels + fine-tune
python 4_export_onnx.py        # -> export/fire_det.onnx
python 5_quantize_espdl.py     # -> export/fire_det_s8_esp32s3.espdl  (+ float vs int8 accuracy check)
```

Every script accepts `--imgsz`, `--epochs`, `--batch`, `--data` and `--device`. Other useful options:
- `python 2_train_student.py --model models/fire-pico.yaml` for the even smaller student
- `python 3_prune.py --ratio 0.4` to prune harder
- `python 5_quantize_espdl.py --calib-dir my_camera_photos` to calibrate with your own camera images

Results go in `runs/`, and each step prints the model's parameters and GFLOPs so you can see it shrink. Check **mAP50** after each step in the training output. Step 5 prints float32 and int8 accuracy side by side.

### Choosing how far to shrink
- Start with the defaults (slim + prune 0.3).
- If mAP50 dropped a lot after pruning, use `--ratio 0.2` or more fine-tune epochs.
- If it's still too slow, try `IMGSZ = 224` or the pico student.

## Improve it with feedback from the device (steps 6–7)

The fire cam firmware lets you judge every result: **B2 = right, B3 = wrong**. After B3 it asks what was really there. Each verdict saves the photo, the model's answer and your verdict to the board's flash, or to a microSD card if you enable one.

1. **Copy the samples to your PC.** This only needs `pip install construct esptool`, not the whole training setup.
   ```
   python pull_field_data.py --wifi 192.168.1.23  # over Wi-Fi: press B2 on the fire cam, use the address it shows
   python pull_field_data.py --port COM5          # over the USB cable (close the serial monitor first)
   python pull_field_data.py --sd E:\FIELD        # from a microSD card, if you use one
   ```
   You can also just open the address in a browser to look through the samples, and download them as a .tar file.
2. **Turn them into training data:** `python 6_add_field_data.py`
   - **Right:** the photo and the model's own boxes are added.
   - **False alarm** (you answered "nothing"): the photo is added as a no-fire example. This is the most useful kind, and it's fully automatic.
   - **Missed fire / missed smoke:** a person has to draw the boxes. The photos are copied to `field_to_label/`. Upload them to Roboflow, draw the boxes, export as YOLOv11, and add the zip once with `python 6_add_field_data.py --labeled export.zip`. After that they're kept for every later run.
   - 20% of your photos are held back to measure the model on **your** camera.
   - Photos pulled twice are only used once.
3. **Fine-tune:** `python 7_finetune_field.py`. This improves your pruned model without changing its size. It prints **false alarms** and **hits** on your held-back photos, before and after.
4. **Export and flash:**
   ```
   python 4_export_onnx.py --weights runs/7_field/weights/best.pt
   python 5_quantize_espdl.py --data data/fire_field.yaml
   ```
   Copy `export/fire_det_s8_esp32s3.espdl` into the firmware as `main/models/fire_det.espdl`, then build and flash.

In Colab, upload the `field_data` folder into the project folder on Drive and use the cells at the end of the notebook.

**How many samples?** Even 30–50 false alarms from your own room make a noticeable difference. Keep collecting and repeat steps 6–7 whenever you have a new batch.

## Using the model on the ESP32-S3 (next stage)

The `.espdl` file uses the same output format as Espressif's own YOLO11n (`box0, score0, box1, score1, box2, score2`). Espressif's `examples/yolo11_detect` in [esp-dl](https://github.com/espressif/esp-dl) can run it:

- Replace its model file with `fire_det_s8_esp32s3.espdl` and set the input size to your `IMGSZ`.
- Set the class names to `fire`, `smoke`, and use a score threshold around 0.3–0.5.
- Store the model in **flash** (rodata or a flash partition), not on an SD card. The S3 CAM's 8 MB PSRAM is needed for the working memory.
- Input normalisation: the image ÷ 255, i.e. mean 0, std 255. This matches the calibration here.

ESP-DL is built with **ESP-IDF**, not the Arduino IDE.

## Troubleshooting

- **CUDA out of memory:** lower `BATCH` in `settings.py`.
- **"Could not find runs/…/best.pt":** run the previous step first.
- **int8 accuracy much lower than float:** use more and more realistic calibration images, and keep `USE_TQT = True`.
- **Package errors:** the scripts rely on Ultralytics internals. Use the versions in `requirements.txt`.

## Tested

The whole pipeline (steps 1–5) was run end to end on a small synthetic fire/smoke dataset with Ultralytics 8.4.160, Torch-Pruning 1.6.1 and ESP-PPQ 1.3.11, producing a valid `.espdl` file:

- Student mAP50: 0.97
- After pruning (51% fewer parameters) and fine-tuning: 0.90
- After int8: 0.91

Steps 6–7 were tested the same way: fake device samples were pulled from both an SD folder and a real wear-levelled flash image, imported, fine-tuned, exported and quantized.

Those numbers only show that the pipeline works. It has **not** yet been run on the ESP32-S3 itself, or on a real fire dataset.

Model format and quantization settings follow Espressif's YOLO11n example:
- [export_onnx.py](https://github.com/espressif/esp-dl/blob/master/models/coco_detect/models/export_onnx.py)
- [quantize_yolo11n](https://github.com/espressif/esp-dl/tree/master/examples/tutorial/how_to_quantize_model/quantize_yolo11n)
