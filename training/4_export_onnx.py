"""Step 4 - Export the pruned model to ONNX in the layout ESP-DL expects.

Output: export/fire_det.onnx with six outputs (box0, score0, box1, score1, box2, score2).
Box decoding and non-max suppression happen on the ESP32 (ESP-DL's YOLO11 post-processor),
which keeps the int8 model accurate.

    python 4_export_onnx.py
    python 4_export_onnx.py --weights runs/2_student/weights/best.pt   # export an unpruned model
"""
import copy
from pathlib import Path

import numpy as np
import torch

from esp_tools import ESP_OUTPUT_NAMES, RUNS, get_args, patch_for_esp_export, print_stats


def extra(p):
    p.add_argument("--weights", default=str(RUNS / "3_pruned" / "weights" / "best.pt"))
    p.add_argument("--out", default="export/fire_det.onnx")


args = get_args(__doc__, extra=extra)

import onnx
import onnxruntime as ort
import onnxsim
from ultralytics import YOLO

if not Path(args.weights).exists():
    raise SystemExit(f"\n[!] {args.weights} not found. Run the earlier steps first.\n")

model = copy.deepcopy(YOLO(args.weights).model).float().cpu().eval()
model = model.fuse(verbose=False)  # merge BatchNorm into convs
print_stats("Exporting", model, args.imgsz)
patch_for_esp_export(model)

out = Path(args.out)
out.parent.mkdir(parents=True, exist_ok=True)
im = torch.zeros(1, 3, args.imgsz, args.imgsz)
torch.onnx.export(
    model, im, str(out),
    opset_version=13,
    input_names=["images"],
    output_names=ESP_OUTPUT_NAMES,
    do_constant_folding=False,
    dynamo=False,
)
m = onnx.load(str(out))
m, ok = onnxsim.simplify(m)
assert ok, "ONNX simplification failed"
names = model.names if isinstance(model.names, dict) else dict(enumerate(model.names))
for k, v in {"imgsz": args.imgsz, "names": names}.items():
    meta = m.metadata_props.add()
    meta.key, meta.value = k, str(v)
onnx.save(onnx.shape_inference.infer_shapes(m), str(out))

# Check ONNX matches PyTorch
x = torch.rand(1, 3, args.imgsz, args.imgsz)
with torch.no_grad():
    ref = [t.numpy() for t in model(x)]
sess = ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])
got = sess.run(None, {"images": x.numpy()})
err = max(float(np.abs(a - b).max()) for a, b in zip(ref, got))
print("Outputs:", ", ".join(f"{o.name}{list(o.shape)}" for o in sess.get_outputs()))
print(f"ONNX vs PyTorch max difference: {err:.2e}")
assert err < 1e-2, "ONNX output does not match PyTorch"
print(f"\nSaved {out} ({out.stat().st_size / 1e6:.2f} MB float32)\nNext: python 5_quantize_espdl.py")
