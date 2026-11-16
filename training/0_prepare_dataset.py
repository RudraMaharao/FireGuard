"""Step 0 - Turn a downloaded fire/smoke dataset (e.g. a Roboflow "YOLOv11" zip) into the layout
the other steps expect, with the class numbers the ESP32 firmware expects:

    0 = fire, 1 = smoke        (any other classes are dropped)

It reads the dataset's own data.yaml to find which number means what, so it does not matter
whether your download calls them "fire"/"Fire"/"flame"/"smoke" or in which order.
Polygon (segmentation) labels are converted to boxes.

    python 0_prepare_dataset.py dataset.zip
    python 0_prepare_dataset.py roboflow1.zip roboflow2.zip --negatives Fire-Smoke-Dataset.zip my_room_photos/

Several datasets are merged. --negatives adds photos with NO fire/smoke (empty labels), which cuts
false alarms; for DeepQuestAI's Fire-Smoke-Dataset only its "Neutral" folder is used, because its
Fire/Smoke photos have no boxes.

Result: datasets/fire/images/{train,val} + labels/{train,val}, and data/fire.yaml.
"""
import argparse
import random
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "datasets" / "fire"
IMG_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def target_class(name: str):
    n = name.lower().replace("_", "-").replace(" ", "-")
    # "fake-fire", "no-fire", "not-fire", "non-fire" ... are things that only LOOK like fire
    # (lamps, glare, pictures of flames): their boxes are dropped, so those photos teach the
    # model what is NOT fire.
    if any(w in n for w in ("fake", "no-fire", "not-fire", "non-fire", "nofire", "false")):
        return None
    if "smoke" in n:
        return 1
    if "fire" in n or "flame" in n:
        return 0
    return None  # other classes (e.g. "person", "candle", a lighter's body) are ignored


def find_split_dirs(base: Path):
    """Returns {"train": images_dir, "val": images_dir, "test": images_dir} for common layouts."""
    found = {}
    for split, names in {"train": ["train"], "val": ["valid", "val", "validation"], "test": ["test"]}.items():
        for n in names:
            for cand in (base / n / "images", base / "images" / n):
                if cand.is_dir():
                    found.setdefault(split, cand)
    return found


def labels_dir_for(images_dir: Path) -> Path:
    parts = list(images_dir.parts)
    i = len(parts) - 1 - parts[::-1].index("images")
    parts[i] = "labels"
    return Path(*parts)


def convert_line(line: str, mapping: dict):
    v = line.split()
    if len(v) < 5:
        return None
    cls = mapping.get(int(float(v[0])))
    if cls is None:
        return None
    nums = list(map(float, v[1:]))
    if len(nums) == 4:
        xc, yc, w, h = nums
    else:  # polygon x1 y1 x2 y2 ... -> bounding box
        xs, ys = nums[0::2], nums[1::2]
        x1, x2, y1, y2 = min(xs), max(xs), min(ys), max(ys)
        xc, yc, w, h = (x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1
    if w <= 0 or h <= 0:
        return None
    return f"{cls} {xc:.6f} {yc:.6f} {w:.6f} {h:.6f}"


def open_source(path: Path, tmp_dirs: list) -> Path:
    """Unzips if needed and returns the folder."""
    path = path.expanduser()
    if not path.exists():
        sys.exit(f"[!] Not found: {path}")
    if path.suffix.lower() == ".zip":
        tmp = Path(tempfile.mkdtemp())
        tmp_dirs.append(tmp)
        print(f"Unzipping {path.name} ...")
        with zipfile.ZipFile(path) as z:
            z.extractall(tmp)
        return tmp
    return path


def load_labelled(path: Path, tag: str, tmp_dirs: list) -> dict:
    """A dataset with boxes (YOLO format + data.yaml). Returns {"train": [...], "val": [...]}."""
    src = open_source(path, tmp_dirs)
    yamls = sorted(src.rglob("data.yaml"), key=lambda p: len(p.parts))  # may sit one folder down
    base = yamls[0].parent if yamls else src
    names = []
    if yamls:
        raw = yaml.safe_load(open(yamls[0])).get("names", [])
        names = [raw[k] for k in sorted(raw)] if isinstance(raw, dict) else list(raw)
    if not names:
        sys.exit(f"[!] {path.name}: no class names (data.yaml) found. Is it a YOLO-format download?")
    mapping = {i: target_class(n) for i, n in enumerate(names)}
    print(f"\n{path.name}: classes -> classes used for training")
    for i, n in enumerate(names):
        t = mapping[i]
        print(f"   {i}: {n:<15} -> {'fire (0)' if t == 0 else 'smoke (1)' if t == 1 else 'ignored'}")
    if not any(t is not None for t in mapping.values()):
        sys.exit(f"[!] {path.name}: no class named fire/flame/smoke.")

    splits = find_split_dirs(base)
    if "train" not in splits:
        sys.exit(f"[!] {path.name}: no train/images folder found")
    pairs = {"train": [], "val": []}
    for split, img_dir in splits.items():
        lab_dir = labels_dir_for(img_dir)
        for img in sorted(p for p in img_dir.iterdir() if p.suffix.lower() in IMG_EXT):
            # the file name gets a prefix so files from different sources/splits never clash
            pairs["train" if split == "train" else "val"].append(
                (img, lab_dir / (img.stem + ".txt"), mapping, f"{tag}_{split}"))
    return pairs


def load_negatives(path: Path, tag: str, tmp_dirs: list) -> dict:
    """Photos WITHOUT fire or smoke (they get empty labels). If the folder has a subfolder called
    "neutral" (like DeepQuestAI's Fire-Smoke-Dataset), only that subfolder is used."""
    src = open_source(path, tmp_dirs)
    neutral = [d for d in src.rglob("*") if d.is_dir() and d.name.lower() == "neutral"]
    roots = neutral or [src]
    images = sorted({p for r in roots for p in r.rglob("*") if p.suffix.lower() in IMG_EXT})
    print(f"\n{path.name}: {len(images)} no-fire images"
          + (f" (from the 'Neutral' folder{'s' if len(neutral) > 1 else ''})" if neutral else ""))
    return {"all": [(img, None, None, f"{tag}_{i:05d}") for i, img in enumerate(images)]}


def split_off_val(items: list) -> tuple:
    random.seed(0)
    items = items[:]
    random.shuffle(items)
    k = max(1, len(items) // 10)
    return items[k:], items[:k]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("datasets", nargs="+", help="one or more YOLO-format datasets (zip or folder) with boxes")
    ap.add_argument("--negatives", nargs="*", default=[],
                    help="zips/folders of photos with NO fire or smoke (added with empty labels)")
    args = ap.parse_args()

    tmp_dirs = []
    pairs = {"train": [], "val": []}
    for n, path in enumerate(args.datasets):
        part = load_labelled(Path(path), f"d{n}", tmp_dirs)
        if not part["val"]:  # this dataset has no validation split: hold out 10% of its train images
            part["train"], part["val"] = split_off_val(part["train"])
        pairs["train"] += part["train"]
        pairs["val"] += part["val"]
    for n, path in enumerate(args.negatives):
        train, val = split_off_val(load_negatives(Path(path), f"neg{n}", tmp_dirs)["all"])
        pairs["train"] += train
        pairs["val"] += val

    if OUT.exists():
        shutil.rmtree(OUT)
    counts = {}
    for split, items in pairs.items():
        (OUT / "images" / split).mkdir(parents=True)
        (OUT / "labels" / split).mkdir(parents=True)
        boxes = [0, 0]
        empty = 0
        for img, lab, mapping, prefix in items:
            name = f"{prefix}_{img.name}" if lab is not None else f"{prefix}{img.suffix.lower()}"
            lines = []
            if lab is not None and lab.exists():
                for line in lab.read_text().splitlines():
                    out = convert_line(line, mapping)
                    if out:
                        lines.append(out)
                        boxes[int(out[0])] += 1
            empty += not lines
            shutil.copy2(img, OUT / "images" / split / name)
            (OUT / "labels" / split / (Path(name).stem + ".txt")).write_text("\n".join(lines) + ("\n" if lines else ""))
        counts[split] = (len(items), boxes, empty)

    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "fire.yaml").write_text(
        "# Written by 0_prepare_dataset.py\n"
        "path: datasets/fire\ntrain: images/train\nval: images/val\n\nnames:\n  0: fire\n  1: smoke\n"
    )
    print("\nDataset ready in datasets/fire:")
    for split, (n, (f, s), e) in counts.items():
        print(f"   {split:<5}: {n:5d} images, {f:6d} fire boxes, {s:6d} smoke boxes, {e:5d} images with no fire/smoke")
    n_train, _, e_train = counts["train"]
    if e_train < n_train * 0.1:
        print("\nTip: fewer than 10% of your training images have no fire/smoke. Add photos of sunsets,\n"
              "orange lamps, candles etc. with --negatives to cut false alarms.")
    print("\nNext: python 1_train_teacher.py")
    for t in tmp_dirs:
        shutil.rmtree(t, ignore_errors=True)


if __name__ == "__main__":
    main()
