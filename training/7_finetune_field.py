"""Step 7 - Fine-tune your finished (pruned) model on the original dataset + the fire cam's field photos.

The model keeps its size (no new pruning) and learns from what you marked right/wrong on the device.
Before and after, it is scored on the field photos that were held back:

  false alarms   no-fire photos where it still reports fire/smoke   (lower is better)
  hits           fire/smoke photos where it finds the right class    (higher is better)

    python 7_finetune_field.py
    python 7_finetune_field.py --epochs 40

Then:  python 4_export_onnx.py --weights runs/7_field/weights/best.pt
       python 5_quantize_espdl.py --data data/fire_field.yaml
and flash the new model (main/models/fire_det.espdl in the firmware).
"""
import copy
from pathlib import Path

import settings
from esp_tools import ROOT, RUNS, get_args, list_images, print_stats, resolve_device


def extra(p):
    p.add_argument("--weights", default=str(RUNS / "3_pruned" / "weights" / "best.pt"),
                   help="model to improve (default: the pruned model from step 3)")
    p.add_argument("--conf", type=float, default=0.45, help="same score threshold as the firmware")
    p.add_argument("--no-distill", action="store_true")


args = get_args(__doc__, epochs_default=30, extra=extra)
if args.data == settings.DATA:
    args.data = "data/fire_field.yaml"

from ultralytics import YOLO
from ultralytics.models.yolo.detect import DetectionTrainer

if not Path(args.data).exists():
    raise SystemExit("\n[!] data/fire_field.yaml not found. Run 6_add_field_data.py first.\n")
if not Path(args.weights).exists():
    raise SystemExit(f"\n[!] {args.weights} not found. Run steps 1-3 first.\n")
teacher = RUNS / "1_teacher" / "weights" / "best.pt"
teacher = None if args.no_distill or not teacher.exists() else str(teacher)


def field_check(weights: str) -> tuple:
    """(false alarms, no-fire photos, hits, fire/smoke photos) on the held-back field photos."""
    model = YOLO(weights)
    fa = neg = hit = pos = 0
    for img in list_images(ROOT / "datasets" / "field" / "images" / "val"):
        lab = img.parent.parent.parent / "labels" / "val" / (img.stem + ".txt")
        truth = {int(l.split()[0]) for l in lab.read_text().splitlines() if l.strip()} if lab.exists() else set()
        res = model.predict(str(img), imgsz=args.imgsz, conf=args.conf, verbose=False)[0]
        found = set(res.boxes.cls.int().tolist())
        if truth:
            pos += 1
            hit += bool(truth & found)
        else:
            neg += 1
            fa += bool(found)
    return fa, neg, hit, pos


def describe(t: tuple) -> str:
    fa, neg, hit, pos = t
    a = f"false alarms {fa}/{neg}" if neg else "no no-fire photos held back"
    b = f"hits {hit}/{pos}" if pos else "no fire/smoke photos held back"
    return f"{a},  {b}"


before = field_check(args.weights)
print(f"\nBefore: {describe(before)}  (on the held-back fire cam photos)\n")

model = copy.deepcopy(YOLO(args.weights).model).float()
print_stats("Model being fine-tuned", model, args.imgsz)


class KeepModelTrainer(DetectionTrainer):
    """Trains the given (pruned) model as it is, instead of rebuilding it from a yaml."""
    given = None

    def get_model(self, cfg=None, weights=None, verbose=True):
        return KeepModelTrainer.given


KeepModelTrainer.given = model
KeepModelTrainer(overrides=dict(
    model=str(args.weights),
    data=args.data,
    imgsz=args.imgsz,
    epochs=args.epochs,
    batch=args.batch,
    device=resolve_device(args.device),
    workers=args.workers,
    project=str(RUNS),
    name="7_field",
    exist_ok=True,
    distill_model=teacher,
    dis=settings.DISTILL_WEIGHT,
    lr0=0.001,          # small steps: improve, don't forget what it knew
    warmup_epochs=0,
)).train()

new = str(RUNS / "7_field" / "weights" / "best.pt")
after = field_check(new)
print(f"\nBefore: {describe(before)}")
print(f"After:  {describe(after)}")
print(f"\nNext:\n   python 4_export_onnx.py --weights {Path(new).relative_to(ROOT)}\n"
      "   python 5_quantize_espdl.py --data data/fire_field.yaml\n"
      "then copy export/fire_det_s8_esp32s3.espdl to the firmware as main/models/fire_det.espdl and flash it.")
