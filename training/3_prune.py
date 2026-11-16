"""Step 3 - Structured pruning (shrinking step 5), then fine-tune with distillation.

Whole channels with the smallest weights are removed from the student, so every layer
really gets thinner (unlike "zeroing" weights, this makes it faster on the ESP32-S3).
The layers ESP-DL needs unchanged (box/score outputs, attention) are left alone.
Then the pruned model is retrained so it recovers its accuracy.

    python 3_prune.py
    python 3_prune.py --ratio 0.4
"""
import copy

import torch

import settings
from esp_tools import (
    RUNS, best_weights, convert_c2f_to_split, esp_detect_forward, get_args,
    model_stats, print_stats, pruning_ignored_layers, resolve_device,
)


def extra(p):
    p.add_argument("--ratio", type=float, default=settings.PRUNE_RATIO, help="fraction of channels to remove")
    p.add_argument("--no-distill", action="store_true", help="fine-tune without the teacher")


args = get_args(__doc__, epochs_default=settings.FINETUNE_EPOCHS, extra=extra)

import torch_pruning as tp
from ultralytics import YOLO
from ultralytics.models.yolo.detect import DetectionTrainer
from ultralytics.nn.modules import Detect

student_path = best_weights("2_student")
teacher = None if args.no_distill else str(best_weights("1_teacher"))

model = copy.deepcopy(YOLO(student_path).model).float().cpu()
before = model_stats(model, args.imgsz)
print_stats("Student before pruning", model, args.imgsz)

# 1) Make C3k2 blocks prunable, and check the maths did not change
model.eval()
x = torch.rand(1, 3, args.imgsz, args.imgsz)
with torch.no_grad():
    ref = model(x)[0]
n = convert_c2f_to_split(model)
with torch.no_grad():
    diff = (model(x)[0] - ref).abs().max().item()
print(f"Converted {n} C3k2 blocks for pruning (output difference {diff:.2e})")
assert diff < 1e-3, "conversion changed the model output"

# 2) Prune. Trace with the ESP-style head so the graph matches what runs on the chip.
detect = [m for m in model.modules() if isinstance(m, Detect)][0]
detect.forward = esp_detect_forward.__get__(detect)
for p in model.parameters():
    p.requires_grad_(True)

pruner = tp.pruner.BasePruner(
    model,
    example_inputs=x,
    importance=tp.importance.GroupMagnitudeImportance(p=2),
    pruning_ratio=args.ratio,
    ignored_layers=pruning_ignored_layers(model),
    round_to=settings.PRUNE_ROUND_TO,
)
pruner.step()
del detect.forward  # back to the normal ultralytics head for training

after = model_stats(model, args.imgsz)
print_stats("Student after pruning", model, args.imgsz)
print(f"Pruning removed {100 * (1 - after[0] / before[0]):.0f}% of parameters "
      f"and {100 * (1 - after[1] / before[1]):.0f}% of compute. Fine-tuning to recover accuracy...\n")


# 3) Fine-tune. The trainer normally rebuilds the model from its yaml, which would undo the
#    pruning, so hand it our pruned model instead.
class PrunedTrainer(DetectionTrainer):
    pruned_model = None

    def get_model(self, cfg=None, weights=None, verbose=True):
        return PrunedTrainer.pruned_model


PrunedTrainer.pruned_model = model
trainer = PrunedTrainer(overrides=dict(
    model=str(student_path),
    data=args.data,
    imgsz=args.imgsz,
    epochs=args.epochs,
    batch=args.batch,
    device=resolve_device(args.device),
    workers=args.workers,
    project=str(RUNS),
    name="3_pruned",
    exist_ok=True,
    distill_model=teacher,
    dis=settings.DISTILL_WEIGHT,
    lr0=0.002,           # gentler than training from scratch
    warmup_epochs=1,
))
trainer.train()

print_stats("Pruned + fine-tuned", YOLO(RUNS / "3_pruned" / "weights" / "best.pt").model.float(), args.imgsz)
print("Next: python 4_export_onnx.py")
