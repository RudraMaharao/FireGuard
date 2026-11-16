"""Step 1 - Train the TEACHER: a bigger YOLO11 (yolo11s) on your fire/smoke data.

It is never put on the ESP32. Its only job is to teach the small model in steps 2 and 3
(knowledge distillation), which wins back accuracy lost by shrinking.

    python 1_train_teacher.py
    python 1_train_teacher.py --epochs 50 --batch 8
"""
import settings
from esp_tools import RUNS, get_args, print_stats, resolve_device

args = get_args(__doc__, epochs_default=settings.TEACHER_EPOCHS)

from ultralytics import YOLO

model = YOLO(settings.TEACHER_WEIGHTS)
model.train(
    data=args.data,
    imgsz=args.imgsz,
    epochs=args.epochs,
    batch=args.batch,
    device=resolve_device(args.device),
    workers=args.workers,
    project=str(RUNS),
    name="1_teacher",
    exist_ok=True,
)
print_stats("Teacher", YOLO(RUNS / "1_teacher" / "weights" / "best.pt").model.float(), args.imgsz)
print(f"Teacher saved to {RUNS / '1_teacher' / 'weights' / 'best.pt'}\nNext: python 2_train_student.py")
