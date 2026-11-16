"""Step 2 - Train the narrow STUDENT (shrinking step 4) with knowledge distillation.

The student is a thinner YOLO11 (models/fire-slim.yaml or fire-pico.yaml, see settings.py).
While it learns from the labels, it is also pushed to copy the teacher's internal features,
which makes a small model noticeably more accurate than training it alone.

    python 2_train_student.py
    python 2_train_student.py --model models/fire-pico.yaml
"""
import settings
from esp_tools import RUNS, best_weights, get_args, print_stats, resolve_device


def extra(p):
    p.add_argument("--model", default=settings.STUDENT_MODEL, help="student yaml (default: %(default)s)")
    p.add_argument("--no-distill", action="store_true", help="train the student without the teacher")


args = get_args(__doc__, epochs_default=settings.STUDENT_EPOCHS, extra=extra)

from ultralytics import YOLO

teacher = None if args.no_distill else str(best_weights("1_teacher"))

model = YOLO(args.model)
print_stats("Student before training", model.model, args.imgsz)
model.train(
    data=args.data,
    imgsz=args.imgsz,
    epochs=args.epochs,
    batch=args.batch,
    device=resolve_device(args.device),
    workers=args.workers,
    project=str(RUNS),
    name="2_student",
    exist_ok=True,
    pretrained=False,
    distill_model=teacher,
    dis=settings.DISTILL_WEIGHT,
)
print_stats("Student", YOLO(RUNS / "2_student" / "weights" / "best.pt").model.float(), args.imgsz)
print("Next: python 3_prune.py")
