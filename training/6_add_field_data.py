"""Step 6 - Turn the fire cam's field samples (your B2 = right / B3 = wrong verdicts) into training data.

Reads every pull in field_data/ (see pull_field_data.py) and sorts each sample by your verdict:

  correct       the model was right -> photo + the model's own boxes become training data
  false_alarm   it saw fire/smoke but there was nothing -> photo becomes a "no fire" example
                (this is the most valuable kind: it directly teaches the model not to repeat it)
  missed_fire   there WAS fire/smoke but the boxes are missing or wrong -> the photo needs
  missed_smoke  boxes drawn by a person, so it is copied to field_to_label/ for you to label

Label the field_to_label/ photos in Roboflow (upload, draw the boxes, export "YOLOv11" zip) and add
them once with --labeled; they are kept in field_data/_labeled and included on every later run.
The same photo pulled twice is only used once, so you can run this any time.

    python 6_add_field_data.py
    python 6_add_field_data.py --labeled roboflow_field_export.zip

Result: datasets/field (20% held back to measure the model on YOUR camera) and data/fire_field.yaml.
Next: python 7_finetune_field.py
"""
import argparse
import csv
import hashlib
import importlib.util
import re
import shutil
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FIELD_DATA = ROOT / "field_data"
OUT = ROOT / "datasets" / "field"
TO_LABEL = ROOT / "field_to_label"


def load_prepare_module():
    spec = importlib.util.spec_from_file_location("prep", ROOT / "0_prepare_dataset.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()[:12]


def is_val(key: str, val_percent: int) -> bool:
    """Deterministic split: the same photo always lands in the same split."""
    return int(key[:8], 16) % 100 < val_percent


def yolo_boxes(txt: Path) -> list:
    """The fire cam writes 'class xc yc w h score'. Training wants 'class xc yc w h'."""
    lines = []
    if txt.exists():
        for line in txt.read_text().splitlines():
            v = line.split()
            if len(v) >= 5:
                lines.append(" ".join(v[:5]))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--labeled", nargs="*", default=[], help="Roboflow YOLO zips of the field_to_label photos")
    ap.add_argument("--val-percent", type=int, default=20, help="share kept aside to measure the model")
    ap.add_argument("--repeat", type=int, default=3,
                    help="copies of each field photo in training: there are few of them, so each counts a bit more")
    args = ap.parse_args()

    samples = {}   # md5 -> (image path, label lines, kind)
    feedback_counts = Counter()
    to_label = []
    for log in sorted(FIELD_DATA.glob("*/LOG.CSV")):
        for row in csv.DictReader(open(log, newline="")):
            img = log.parent / row["image"]
            if not img.exists():
                continue
            key = md5(img)
            if key in samples or any(k == key for k, _ in to_label):
                continue  # the same photo from an earlier pull
            fb = row["feedback"].strip()
            feedback_counts[fb] += 1
            if fb == "correct":
                samples[key] = (img, yolo_boxes(img.with_suffix(".TXT")), "correct")
            elif fb == "false_alarm":
                samples[key] = (img, [], "false_alarm")
            elif fb in ("missed_fire", "missed_smoke"):
                to_label.append((key, (img, fb)))

    # Photos you labeled in Roboflow: stored once in field_data/_labeled, then re-read on every run
    labeled_dir = FIELD_DATA / "_labeled"
    if args.labeled:
        prep = load_prepare_module()
        tmp_dirs = []
        labeled_dir.mkdir(parents=True, exist_ok=True)
        added = 0
        for n, z in enumerate(args.labeled):
            part = prep.load_labelled(Path(z), f"lab{n}", tmp_dirs)
            for img, lab, mapping, _ in part["train"] + part["val"]:
                lines = []
                if lab.exists():
                    lines = [o for line in lab.read_text().splitlines() if (o := prep.convert_line(line, mapping))]
                m = re.search(r"field_([0-9a-f]{12})", img.name)  # the name we gave it in field_to_label/
                key = m.group(1) if m else md5(img)
                shutil.copy2(img, labeled_dir / f"{key}{img.suffix.lower()}")
                (labeled_dir / f"{key}.TXT").write_text("\n".join(lines) + ("\n" if lines else ""))
                added += 1
        for t in tmp_dirs:
            shutil.rmtree(t, ignore_errors=True)
        print(f"Stored {added} labeled photos in {labeled_dir.relative_to(ROOT)}")
    for img in sorted(labeled_dir.glob("*")) if labeled_dir.exists() else []:
        if img.suffix.lower() in (".jpg", ".jpeg", ".png", ".bmp", ".webp"):
            samples[img.stem] = (img, yolo_boxes(img.with_suffix(".TXT")), "labeled")

    if not samples and not to_label:
        sys.exit("[!] No field samples found in field_data/. Run pull_field_data.py first.")

    # Build datasets/field from scratch every time (it is cheap and keeps it consistent)
    if OUT.exists():
        shutil.rmtree(OUT)
    for split in ("train", "val"):
        (OUT / "images" / split).mkdir(parents=True)
        (OUT / "labels" / split).mkdir(parents=True)
    n_split = Counter()
    kinds = Counter()
    for key, (img, lines, kind) in samples.items():
        split = "val" if is_val(key, args.val_percent) else "train"
        copies = 1 if split == "val" else args.repeat
        for c in range(copies):
            name = f"field_{key}_{c}"
            shutil.copy2(img, OUT / "images" / split / f"{name}{img.suffix.lower()}")
            (OUT / "labels" / split / f"{name}.txt").write_text("\n".join(lines) + ("\n" if lines else ""))
        n_split[split] += 1
        kinds[kind] += 1

    # Photos that still need boxes
    pending = [(k, v) for k, v in to_label if not (k in samples and samples[k][2] == "labeled")]
    if TO_LABEL.exists():
        shutil.rmtree(TO_LABEL)
    for key, (img, fb) in pending:
        d = TO_LABEL / fb.replace("missed_", "")
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy2(img, d / f"field_{key}{img.suffix.lower()}")

    # Dataset yaml: your original dataset + field photos
    (ROOT / "data" / "fire_field.yaml").write_text(
        "# Written by 6_add_field_data.py: original dataset + photos from the fire cam\n"
        "path: datasets\n"
        "train: [fire/images/train, field/images/train]\n"
        "val: [fire/images/val, field/images/val]\n\n"
        "names:\n  0: fire\n  1: smoke\n"
    )
    (ROOT / "data" / "field_only.yaml").write_text(
        "# Written by 6_add_field_data.py: only the held-back fire cam photos (to measure the model on them)\n"
        "path: datasets/field\ntrain: images/train\nval: images/val\n\nnames:\n  0: fire\n  1: smoke\n"
    )

    total = sum(feedback_counts.values())
    print("\nYour verdicts on the fire cam:")
    for fb in ("correct", "false_alarm", "missed_fire", "missed_smoke"):
        print(f"   {fb:<13} {feedback_counts[fb]:4d}")
    if total:
        print(f"   -> the model was right {100 * feedback_counts['correct'] / total:.0f}% of the time")
    print(f"\nAdded to datasets/field: {sum(kinds.values())} photos "
          f"({kinds['correct']} correct, {kinds['false_alarm']} false alarms as no-fire examples, "
          f"{kinds['labeled']} labeled by you)")
    print(f"   train: {n_split['train']} photos (x{args.repeat} copies)   val (held back): {n_split['val']} photos")
    if pending:
        print(f"\n{len(pending)} photos need boxes: see {TO_LABEL.relative_to(ROOT)}/ (fire/ and smoke/).")
        print("   Upload them to Roboflow, draw the boxes, export as YOLOv11 zip, then:")
        print("   python 6_add_field_data.py --labeled that_export.zip")
    print("\nNext: python 7_finetune_field.py")


if __name__ == "__main__":
    main()
