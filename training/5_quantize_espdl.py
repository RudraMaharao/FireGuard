"""Step 5 - int8 quantization for the ESP32-S3 (shrinking step 2) with Espressif's ESP-PPQ.

Weights and activations become 8-bit integers: about 4x smaller than float32, and the
ESP32-S3's vector unit runs int8 maths far faster. A few hundred of your own images are
used to measure the value ranges ("calibration"), so use real photos from your camera
setup if you can.

Output: export/fire_det_s8_esp32s3.espdl  -> the file you flash with ESP-DL.

    python 5_quantize_espdl.py
    python 5_quantize_espdl.py --calib-dir my_camera_photos
"""
import random
from pathlib import Path

import torch
from torch.utils.data import DataLoader, Dataset

import settings
from esp_tools import (ESP_OUTPUT_NAMES, dataset_image_dirs, evaluate_map, get_args, letterbox,
                       list_images, load_val_samples)


def extra(p):
    p.add_argument("--onnx", default="export/fire_det.onnx")
    p.add_argument("--calib-dir", default=None, help="folder of calibration images (default: your dataset)")
    p.add_argument("--no-tqt", action="store_true", help="skip TQT fine-tuning (faster, a bit less accurate)")
    p.add_argument("--no-eval", action="store_true", help="skip the float vs int8 accuracy check")


args = get_args(__doc__, extra=extra)

import onnx
from PIL import Image

onnx_path = Path(args.onnx)
if not onnx_path.exists():
    raise SystemExit(f"\n[!] {onnx_path} not found. Run 4_export_onnx.py first.\n")
imgsz = args.imgsz
for p in onnx.load(str(onnx_path)).metadata_props:  # use the size the model was exported at
    if p.key == "imgsz":
        imgsz = int(p.value)
target = settings.TARGET
out = onnx_path.with_name(f"{onnx_path.stem}_s8_{target}.espdl")

# ---- calibration images
if args.calib_dir:
    images = list_images(Path(args.calib_dir))
else:
    images = [p for d in dataset_image_dirs(args.data) for p in list_images(d)]
if not images:
    raise SystemExit("\n[!] No calibration images found.\n")
random.seed(0)
images = random.sample(images, min(settings.CALIB_IMAGES, len(images)))
print(f"Calibrating with {len(images)} images at {imgsz}x{imgsz}, target {target}")


class CalibSet(Dataset):
    def __len__(self):
        return len(images)

    def __getitem__(self, i):
        img = letterbox(Image.open(images[i]).convert("RGB"), imgsz)
        t = torch.from_numpy(__import__("numpy").asarray(img, dtype="float32") / 255.0)
        return t.permute(2, 0, 1)  # CHW, 0-1 range (the ESP32 side divides by 255 too)


BATCH = 8
loader = DataLoader(CalibSet(), batch_size=BATCH, shuffle=False)

# ---- quantize (settings follow Espressif's YOLO11n example)
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx

qs = QuantizationSettingFactory.espdl_setting()
qs.quantize_activation_setting.calib_algorithm = "percentile"
qs.bias_correct = True
qs.bias_correct_setting.block_size = 4
qs.bias_correct_setting.steps = 32
qs.tqt_optimization = settings.USE_TQT and not args.no_tqt
if qs.tqt_optimization:
    t = qs.tqt_optimization_setting
    t.lr = 1e-5
    t.steps = settings.TQT_STEPS
    t.block_size = 4
    t.is_scale_trainable = True
    t.gamma = 0.0
    t.int_lambda = 0.0
    t.collecting_device = "cpu"

graph = espdl_quantize_onnx(
    onnx_import_file=str(onnx_path),
    espdl_export_file=str(out),
    calib_dataloader=loader,
    calib_steps=max(1, len(images) // BATCH),
    input_shape=[1, 3, imgsz, imgsz],
    target=target,
    num_of_bits=8,
    collate_fn=lambda b: b.to("cpu"),
    setting=qs,
    device="cpu",
    error_report=True,
    skip_export=False,
    export_test_values=False,
    verbose=0,
)
print(f"\nSaved {out} ({out.stat().st_size / 1e6:.2f} MB)")

# ---- accuracy check: float model vs the int8 model (simulated exactly as quantized)
if not args.no_eval:
    samples, names = load_val_samples(args.data, imgsz, limit=200)
    if samples:
        import onnxruntime as ort
        from esp_ppq.executor import TorchExecutor

        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        executor = TorchExecutor(graph=graph, device="cpu")
        float_fn = lambda x: sess.run(ESP_OUTPUT_NAMES, {"images": x.numpy()})
        int8_fn = lambda x: [t.cpu() for t in executor.forward(inputs=x, output_names=ESP_OUTPUT_NAMES)]
        f50, f5095 = evaluate_map(float_fn, samples, imgsz, names)
        q50, q5095 = evaluate_map(int8_fn, samples, imgsz, names)
        print(f"\nAccuracy on {len(samples)} validation images:")
        print(f"  float32 : mAP50 {f50:.3f}   mAP50-95 {f5095:.3f}")
        print(f"  int8    : mAP50 {q50:.3f}   mAP50-95 {q5095:.3f}")
        if f50 > 0 and q50 < f50 - 0.05:
            print("  int8 lost more than 5 points: calibrate with more / more realistic images,\n"
                  "  or keep USE_TQT = True.")

print("\nIn the error report above, layers under ~10% are fine; big numbers near the outputs mean\n"
      "you should calibrate with more (and more realistic) images.")
