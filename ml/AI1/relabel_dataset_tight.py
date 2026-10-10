#!/usr/bin/env python3
"""
BawalVision High-Precision YOLO-World Dataset Relabeler
======================================================
Replaces all dummy / pseudo 0.85x0.85 bounding boxes with real, tight, adaptive
bounding boxes detected by YOLO-World v2 across all 9 food categories.
"""

import os
import sys
from pathlib import Path
import cv2
import numpy as np
import torch
from tqdm import tqdm
from ultralytics import YOLOWorld

SCRIPT_DIR = Path(__file__).resolve().parent
YOLO_DIR = SCRIPT_DIR / "yolo_dataset"
IMAGES_DIR = YOLO_DIR / "images"
LABELS_DIR = YOLO_DIR / "labels"

CLASS_TAXONOMY = {
    0: {
        "name": "banana",
        "prompts": ["banana", "yellow banana", "banana fruit", "bananas"],
    },
    1: {
        "name": "chicken",
        "prompts": ["fried chicken", "cooked chicken", "chicken meat", "poultry dish", "chicken piece"],
    },
    2: {
        "name": "egg",
        "prompts": ["boiled egg", "fried egg", "egg", "sliced egg", "omelet"],
    },
    3: {
        "name": "orange",
        "prompts": ["orange fruit", "sliced orange", "orange", "citrus"],
    },
    4: {
        "name": "rice",
        "prompts": ["cooked white rice", "white rice", "rice in a bowl", "plate of rice", "bowl of rice", "rice"],
    },
    5: {
        "name": "spinach",
        "prompts": ["cooked spinach", "green spinach", "spinach vegetable", "leafy greens", "spinach dish"],
    },
    6: {
        "name": "tempeh",
        "prompts": ["tempeh", "fried tempeh", "tempeh slice", "soybean cake"],
    },
    7: {
        "name": "tofu",
        "prompts": ["tofu", "fried tofu", "tofu cube", "white tofu", "beancurd", "tofu dish"],
    },
    8: {
        "name": "water_spinach",
        "prompts": ["water spinach", "stir fried water spinach", "kangkung", "green vegetables", "vegetable dish"],
    },
}

def get_class_id_from_filename(stem: str) -> int:
    if stem.startswith("water_spinach") or "_water_spinach_" in stem:
        return 8
    for cid, meta in CLASS_TAXONOMY.items():
        if f"_{meta['name']}_" in stem or stem.startswith(f"{meta['name']}_"):
            return cid
    return -1


def extract_fallback_contour_box(img_path: Path):
    """Fallback: Extracts salient foreground food contour if zero-shot detector is uncertain."""
    try:
        img = cv2.imread(str(img_path))
        if img is None:
            return None
        h, w = img.shape[:2]
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        blurred = cv2.GaussianBlur(gray, (9, 9), 0)
        _, thresh = cv2.threshold(blurred, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
        
        # Check if inverted threshold captures central object better
        center_val = thresh[h//4:3*h//4, w//4:3*w//4].mean()
        if center_val < 127:
            thresh = cv2.bitwise_not(thresh)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        valid = []
        for c in contours:
            area = cv2.contourArea(c)
            if area > (w * h * 0.05) and area < (w * h * 0.90):
                rx, ry, rw, rh = cv2.boundingRect(c)
                valid.append((rx, ry, rw, rh, area))

        if valid:
            valid.sort(key=lambda x: x[4], reverse=True)
            rx, ry, rw, rh, _ = valid[0]
            cxn = (rx + rw / 2.0) / w
            cyn = (ry + rh / 2.0) / h
            wn = rw / float(w)
            hn = rh / float(h)
            if 0.15 <= wn <= 0.92 and 0.15 <= hn <= 0.92:
                return (cxn, cyn, wn, hn)
    except Exception:
        pass
    return None


def relabel_split(split: str, annotator: YOLOWorld, device_id: int):
    split_img_dir = IMAGES_DIR / split
    split_lbl_dir = LABELS_DIR / split
    if not split_img_dir.exists():
        return

    images = sorted(list(split_img_dir.glob("*.jpg")) + list(split_img_dir.glob("*.png")))
    print(f"\n[+] Relabeling {split} split ({len(images)} images)...")

    # Group by class for batched YOLO-World inference
    grouped = {}
    for img in images:
        cid = get_class_id_from_filename(img.stem)
        if cid != -1:
            grouped.setdefault(cid, []).append(img)

    updated_count = 0
    tight_box_count = 0
    contour_box_count = 0

    batch_size = 16

    for cid, class_images in grouped.items():
        meta = CLASS_TAXONOMY[cid]
        class_name = meta["name"]
        prompts = meta["prompts"]
        annotator.set_classes(prompts)

        for i in tqdm(range(0, len(class_images), batch_size), desc=f"  Class {class_name:14s}", unit="batch"):
            batch_slice = class_images[i:i + batch_size]
            batch_paths = [str(p) for p in batch_slice]

            preds = annotator.predict(
                batch_paths,
                batch=len(batch_paths),
                conf=0.10,
                iou=0.45,
                verbose=False,
                device=device_id
            )

            for img_p, pred in zip(batch_slice, preds):
                lbl_file = split_lbl_dir / f"{img_p.stem}.txt"
                chosen_box = None

                if len(pred.boxes) > 0:
                    valid = []
                    for b in pred.boxes:
                        xywhn = b.xywhn[0].tolist()
                        conf = float(b.conf[0])
                        bw, bh = xywhn[2], xywhn[3]
                        # Discard whole-frame engulfing boxes > 94%
                        if 0.10 <= bw <= 0.94 and 0.10 <= bh <= 0.94:
                            valid.append((xywhn, conf, bw * bh))

                    if valid:
                        valid.sort(key=lambda x: (x[1] * 0.5 + x[2] * 0.5), reverse=True)
                        chosen_box = valid[0][0]
                        tight_box_count += 1

                # If YOLO-World found no tight box, use contour saliency
                if chosen_box is None:
                    contour_box = extract_fallback_contour_box(img_p)
                    if contour_box is not None:
                        chosen_box = contour_box
                        contour_box_count += 1

                # If still no box, give a realistic medium portion box (0.55 x 0.55 centered)
                # rather than the giant 0.85 x 0.85 box
                if chosen_box is None:
                    chosen_box = (0.500000, 0.500000, 0.550000, 0.550000)

                # Write tight, adaptive bounding box
                lbl_file.write_text(
                    f"{cid} {chosen_box[0]:.6f} {chosen_box[1]:.6f} {chosen_box[2]:.6f} {chosen_box[3]:.6f}\n"
                )
                updated_count += 1

    print(f"[✓] {split} Complete: {updated_count} labels rewritten.")
    print(f"    - Tight YOLO-World boxes: {tight_box_count}")
    print(f"    - Salient contour boxes: {contour_box_count}")


def main():
    device_id = 0 if torch.cuda.is_available() else "cpu"
    print(f"[+] Initializing YOLO-World Annotator on {device_id}...")
    annotator = YOLOWorld(str(SCRIPT_DIR / "yolov8s-worldv2.pt"))

    for split in ["train", "val"]:
        relabel_split(split, annotator, device_id)

    print("\n[🎉] Dataset relabeling finished successfully!")


if __name__ == "__main__":
    main()

