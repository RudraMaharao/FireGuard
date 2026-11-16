"""Shared helpers for the fire-detection YOLO11 -> ESP32-S3 pipeline.

Nothing in here needs editing. Settings live in settings.py.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import torch
import torch.nn as nn

import settings

ROOT = Path(__file__).resolve().parent
RUNS = ROOT / "runs"


# ----------------------------------------------------------------------------------------------
# Command line: every script accepts the same overrides, defaults come from settings.py
# ----------------------------------------------------------------------------------------------
def get_args(description: str, epochs_default: int | None = None, extra=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=description)
    if extra:
        extra(p)
    p.add_argument("--data", default=settings.DATA, help="dataset yaml (default: %(default)s)")
    p.add_argument("--imgsz", type=int, default=settings.IMGSZ, help="square input size (default: %(default)s)")
    p.add_argument("--epochs", type=int, default=epochs_default, help="training epochs")
    p.add_argument("--batch", type=int, default=settings.BATCH, help="batch size (default: %(default)s)")
    p.add_argument("--device", default=settings.DEVICE, help="'0' for first GPU, 'cpu', or '' for auto")
    p.add_argument("--workers", type=int, default=settings.WORKERS)
    return p.parse_args()


def resolve_device(device: str) -> str:
    if device:
        return device
    return "0" if torch.cuda.is_available() else "cpu"


def best_weights(run_name: str) -> Path:
    """Path of best.pt produced by a previous step, with a clear error if it is missing."""
    path = RUNS / run_name / "weights" / "best.pt"
    if not path.exists():
        sys.exit(f"\n[!] Could not find {path}\n    Run the previous step first.\n")
    return path


# ----------------------------------------------------------------------------------------------
# Model size report
# ----------------------------------------------------------------------------------------------
def model_stats(model: nn.Module, imgsz: int) -> tuple[float, float]:
    """Returns (million parameters, GFLOPs at imgsz x imgsz)."""
    from ultralytics.utils.torch_utils import get_flops

    params = sum(p.numel() for p in model.parameters()) / 1e6
    return params, get_flops(model, imgsz)


def print_stats(title: str, model: nn.Module, imgsz: int) -> None:
    params, gflops = model_stats(model, imgsz)
    print(f"\n=== {title}: {params:.3f} M parameters, {gflops:.2f} GFLOPs at {imgsz}x{imgsz} ===\n")


# ----------------------------------------------------------------------------------------------
# Pruning support: C3k2 / C2f split their first conv with chunk(), which channel pruning cannot
# follow. C2fSplit does the same maths with two separate convs, so it can be pruned safely.
# ----------------------------------------------------------------------------------------------
class C2fSplit(nn.Module):
    """Drop-in replacement for C2f / C3k2 that uses two convs instead of one conv + chunk()."""

    def __init__(self):
        super().__init__()

    def forward(self, x):
        y = [self.cv0(x), self.cv1(x)]
        y.extend(m(y[-1]) for m in self.m)
        return self.cv2(torch.cat(y, 1))


def _half_conv(conv_module, part: int):
    """Copy of an ultralytics Conv keeping only the first (part=0) or second (part=1) half of its outputs."""
    import copy

    new = copy.deepcopy(conv_module)
    c = conv_module.conv.out_channels // 2
    sl = slice(part * c, (part + 1) * c)
    new.conv.out_channels = c
    new.conv.weight = nn.Parameter(conv_module.conv.weight.data[sl].clone())
    if conv_module.conv.bias is not None:
        new.conv.bias = nn.Parameter(conv_module.conv.bias.data[sl].clone())
    bn = conv_module.bn
    new.bn.num_features = c
    new.bn.weight = nn.Parameter(bn.weight.data[sl].clone())
    new.bn.bias = nn.Parameter(bn.bias.data[sl].clone())
    new.bn.running_mean = bn.running_mean.data[sl].clone()
    new.bn.running_var = bn.running_var.data[sl].clone()
    return new


def convert_c2f_to_split(model: nn.Module) -> int:
    """Replaces every C2f / C3k2 block in-place. Returns how many were converted."""
    from ultralytics.nn.modules.block import C2f

    count = 0
    for parent in list(model.modules()):
        for name, child in list(parent.named_children()):
            if isinstance(child, C2f) and type(child).forward is C2f.forward:
                new = C2fSplit()
                new.c = child.c
                new.cv0 = _half_conv(child.cv1, 0)
                new.cv1 = _half_conv(child.cv1, 1)
                new.cv2 = child.cv2
                new.m = child.m
                # keep the attributes ultralytics uses for bookkeeping (i, f, type, np)
                for attr in ("i", "f", "type", "np"):
                    if hasattr(child, attr):
                        setattr(new, attr, getattr(child, attr))
                setattr(parent, name, new)
                count += 1
    return count


def pruning_ignored_layers(model: nn.Module) -> list[nn.Module]:
    """Layers whose OUTPUT channels must stay fixed for ESP-DL:
    - the last conv of each box branch (4 x 16 DFL bins = 64 channels)
    - the last conv of each class branch (one channel per class)
    - the DFL layer
    - the attention block (its head count is baked into reshapes)
    """
    from ultralytics.nn.modules import C2PSA, Detect

    ignored = []
    for m in model.modules():
        if isinstance(m, Detect):
            for i in range(m.nl):
                ignored.append(m.cv2[i][-1])
                ignored.append(m.cv3[i][-1])
            if hasattr(m, "dfl"):
                ignored.append(m.dfl)
        if isinstance(m, C2PSA):
            ignored.append(m)
    return ignored


# ----------------------------------------------------------------------------------------------
# ESP-DL export format (matches espressif/esp-dl models/coco_detect/models/export_onnx.py):
# six raw outputs box0, score0, box1, score1, box2, score2 - decoding happens on the ESP32.
# ----------------------------------------------------------------------------------------------
ESP_OUTPUT_NAMES = ["box0", "score0", "box1", "score1", "box2", "score2"]


def esp_detect_forward(self, x):
    return (
        self.cv2[0](x[0]), self.cv3[0](x[0]),
        self.cv2[1](x[1]), self.cv3[1](x[1]),
        self.cv2[2](x[2]), self.cv3[2](x[2]),
    )


def esp_attention_forward(self, x):
    B, C, H, W = x.shape
    N = H * W
    qkv = self.qkv(x)
    q, k, v = qkv.view(-1, self.num_heads, self.key_dim * 2 + self.head_dim, N).split(
        [self.key_dim, self.key_dim, self.head_dim], dim=2
    )
    attn = (q.transpose(-2, -1) @ k) * self.scale
    attn = attn.softmax(dim=-1)
    x = (v @ attn.transpose(-2, -1)).view(-1, C, H, W) + self.pe(v.reshape(-1, C, H, W))
    return self.proj(x)


def patch_for_esp_export(model: nn.Module) -> None:
    from ultralytics.nn.modules import Detect
    from ultralytics.nn.modules.block import Attention

    for m in model.modules():
        if isinstance(m, Attention):
            m.forward = esp_attention_forward.__get__(m)
        if isinstance(m, Detect):
            m.forward = esp_detect_forward.__get__(m)


# ----------------------------------------------------------------------------------------------
# Image helpers for calibration
# ----------------------------------------------------------------------------------------------
def letterbox(img, size: int):
    """Resize keeping aspect ratio and pad to size x size with grey (114), like YOLO and ESP-DL."""
    from PIL import Image

    w, h = img.size
    r = min(size / w, size / h)
    nw, nh = max(1, round(w * r)), max(1, round(h * r))
    img = img.resize((nw, nh), Image.BILINEAR)
    canvas = Image.new("RGB", (size, size), (114, 114, 114))
    canvas.paste(img, ((size - nw) // 2, (size - nh) // 2))
    return canvas


def list_images(folder: Path) -> list[Path]:
    exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    return sorted(p for p in Path(folder).rglob("*") if p.suffix.lower() in exts)


def dataset_image_dirs(data_yaml: str, keys=("train", "val")) -> list[Path]:
    """Image folders listed in the dataset yaml (each key may be one folder or a list)."""
    import yaml

    d = yaml.safe_load(open(data_yaml))
    base = Path(d.get("path", "."))
    if not base.is_absolute():
        base = (ROOT / base).resolve()  # same rule Ultralytics uses: relative to the project folder
    dirs = []
    for key in keys:
        for item in d.get(key) if isinstance(d.get(key), list) else [d.get(key)]:
            if item:
                p = Path(item)
                dirs.append(p if p.is_absolute() else base / p)
    return dirs


os.chdir(ROOT)  # every script works no matter where it is started from


# ----------------------------------------------------------------------------------------------
# Accuracy check for the exported / quantized model (decodes the ESP-format outputs the same
# way ESP-DL's YOLO11 post-processor does on the chip)
# ----------------------------------------------------------------------------------------------
def decode_esp_outputs(outs, imgsz: int, conf: float = 0.001, iou: float = 0.7, max_det: int = 300):
    """outs: [box0, score0, box1, score1, box2, score2] for ONE image (numpy or torch, NCHW).
    Returns tensor (n, 6): x1, y1, x2, y2, score, class in letterboxed-image pixels."""
    import torchvision

    boxes, scores = [], []
    for i in range(3):
        box = torch.as_tensor(outs[2 * i]).float()[0]        # (64, H, W)
        cls = torch.as_tensor(outs[2 * i + 1]).float()[0]    # (nc, H, W)
        _, H, W = cls.shape
        stride = imgsz / H
        dist = box.view(4, 16, H, W).softmax(1)
        dist = (dist * torch.arange(16, dtype=torch.float32).view(1, 16, 1, 1)).sum(1)  # (4, H, W)
        gy, gx = torch.meshgrid(torch.arange(H), torch.arange(W), indexing="ij")
        cx, cy = (gx + 0.5) * stride, (gy + 0.5) * stride
        xyxy = torch.stack([cx - dist[0] * stride, cy - dist[1] * stride,
                            cx + dist[2] * stride, cy + dist[3] * stride], -1).view(-1, 4)
        boxes.append(xyxy)
        scores.append(cls.sigmoid().permute(1, 2, 0).reshape(-1, cls.shape[0]))
    boxes, scores = torch.cat(boxes), torch.cat(scores)
    best, labels = scores.max(1)
    keep = best > conf
    boxes, best, labels = boxes[keep], best[keep], labels[keep]
    keep = torchvision.ops.batched_nms(boxes, best, labels, iou)[:max_det]
    return torch.cat([boxes[keep], best[keep, None], labels[keep, None].float()], 1)


def load_val_samples(data_yaml: str, imgsz: int, limit: int = 200):
    """Letterboxed val images (CHW float 0-1) with their labels as (cls, x1, y1, x2, y2) pixels."""
    import numpy as np
    import yaml
    from PIL import Image

    d = yaml.safe_load(open(data_yaml))
    val_images = [p for vd in dataset_image_dirs(data_yaml, keys=("val",)) for p in list_images(vd)]
    if len(val_images) > limit:  # spread the check over all val folders (e.g. dataset + field photos)
        step = len(val_images) / limit
        val_images = [val_images[int(i * step)] for i in range(limit)]
    samples = []
    for img_path in val_images:
        img = Image.open(img_path).convert("RGB")
        w, h = img.size
        r = min(imgsz / w, imgsz / h)
        padx, pady = (imgsz - round(w * r)) // 2, (imgsz - round(h * r)) // 2
        x = torch.from_numpy(np.asarray(letterbox(img, imgsz), dtype="float32") / 255.0).permute(2, 0, 1)
        parts = list(img_path.parts)
        idx = len(parts) - 1 - parts[::-1].index("images")
        parts[idx] = "labels"
        label_path = Path(*parts).with_suffix(".txt")
        gt = []
        if label_path.exists():
            for line in label_path.read_text().split("\n"):
                v = line.split()
                if len(v) >= 5:
                    c, xc, yc, bw, bh = int(v[0]), *map(float, v[1:5])
                    gt.append([c, (xc - bw / 2) * w * r + padx, (yc - bh / 2) * h * r + pady,
                               (xc + bw / 2) * w * r + padx, (yc + bh / 2) * h * r + pady])
        samples.append((x, torch.tensor(gt, dtype=torch.float32).view(-1, 5)))
    return samples, d.get("names", {})


def evaluate_map(predict_fn, samples, imgsz: int, names) -> tuple[float, float]:
    """predict_fn(x[1,3,H,W]) -> ESP outputs. Returns (mAP50, mAP50-95)."""
    import numpy as np
    from torchvision.ops import box_iou
    from ultralytics.utils.metrics import ap_per_class

    iouv = torch.linspace(0.5, 0.95, 10)
    stats = []
    for x, gt in samples:
        pred = decode_esp_outputs(predict_fn(x[None]), imgsz)
        correct = torch.zeros(len(pred), 10, dtype=torch.bool)
        if len(pred) and len(gt):
            iou = box_iou(gt[:, 1:], pred[:, :4])
            same = gt[:, :1] == pred[:, 5]
            for j, t in enumerate(iouv):
                m = torch.nonzero((iou >= t) & same)
                if len(m):
                    m = torch.cat([m, iou[m[:, 0], m[:, 1]][:, None]], 1).numpy()
                    m = m[m[:, 2].argsort()[::-1]]
                    m = m[np.unique(m[:, 1], return_index=True)[1]]
                    m = m[np.unique(m[:, 0], return_index=True)[1]]
                    correct[m[:, 1].astype(int), j] = True
        stats.append((correct, pred[:, 4], pred[:, 5], gt[:, 0]))
    tp, conf, pcls, tcls = (torch.cat(s).numpy() for s in zip(*stats))
    if not len(tcls):
        return 0.0, 0.0
    ap = ap_per_class(tp, conf, pcls, tcls, names=names)[5]  # (classes, 10)
    return float(ap[:, 0].mean()), float(ap.mean())
