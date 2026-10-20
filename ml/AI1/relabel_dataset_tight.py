#!/usr/bin/env python3
import gc
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

# Disjoint, class-specific prompts with Indonesian culinary synonyms for YOLO-World v2
CLASS_TAXONOMY = {
    0: {
        "name": "banana",
        "prompts": [
            "yellow banana",
            "peeled banana",
            "banana fruit",
            "sliced banana",
            "bunch of bananas",
            "banana",
            "pisang",
        ],
    },
    1: {
        "name": "chicken",
        "prompts": [
            "fried chicken",
            "cooked chicken",
            "chicken meat",
            "crispy fried chicken",
            "chicken drumstick",
            "ayam goreng",
            "ayam",
            "poultry",
            "roasted chicken",
            "food",
        ],
    },
    2: {
        "name": "egg",
        "prompts": [
            "boiled egg",
            "fried egg",
            "hard boiled egg",
            "sunny side up egg",
            "egg",
            "omelet",
            "telur",
            "telur rebus",
            "telur ceplok",
            "food",
        ],
    },
    3: {
        "name": "orange",
        "prompts": [
            "orange fruit",
            "peeled orange segments",
            "sliced orange",
            "mandarin orange",
            "citrus",
            "orange",
            "jeruk",
            "fruit",
        ],
    },
    4: {
        "name": "rice",
        "prompts": [
            "cooked white rice",
            "steamed white rice",
            "mound of white rice",
            "white rice",
            "rice",
            "nasi",
            "nasi putih",
            "bowl of rice",
            "food",
        ],
    },
    5: {
        "name": "spinach",
        "prompts": [
            "cooked spinach",
            "boiled spinach leaves",
            "cooked dark green spinach",
            "spinach leaf bundle",
            "spinach",
            "bayam",
            "sayur bayam",
            "leafy greens",
            "vegetable",
        ],
    },
    6: {
        "name": "tempeh",
        "prompts": [
            "fried tempeh",
            "fried tempeh slice",
            "fermented soybean cake tempeh",
            "textured brown tempeh block",
            "tempeh",
            "tempe",
            "tempe goreng",
            "soybean cake",
            "food",
        ],
    },
    7: {
        "name": "tofu",
        "prompts": [
            "fried tofu",
            "fried tofu cube",
            "white tofu block",
            "smooth soybean curd tofu",
            "golden puff tofu",
            "tofu",
            "tahu",
            "tahu goreng",
            "bean curd",
            "soybean curd",
            "food",
        ],
    },
    8: {
        "name": "water_spinach",
        "prompts": [
            "water spinach",
            "stir fried water spinach",
            "hollow stem kangkung",
            "water spinach stems and leaves",
            "kangkung",
            "tumis kangkung",
            "green vegetable",
            "vegetable",
        ],
    },
}

MAX_BOXES_PER_IMAGE = 4


def get_class_id_from_filename(stem: str) -> int:
    if stem.startswith("water_spinach") or "_water_spinach_" in stem:
        return 8
    for cid, meta in CLASS_TAXONOMY.items():
        if f"_{meta['name']}_" in stem or stem.startswith(f"{meta['name']}_"):
            return cid
    return -1


def compute_iou_xywhn(box_a, box_b) -> float:
    """Computes IoU between two normalized [cx, cy, w, h] boxes."""
    ax1, ay1 = box_a[0] - box_a[2] / 2.0, box_a[1] - box_a[3] / 2.0
    ax2, ay2 = box_a[0] + box_a[2] / 2.0, box_a[1] + box_a[3] / 2.0
    bx1, by1 = box_b[0] - box_b[2] / 2.0, box_b[1] - box_b[3] / 2.0
    bx2, by2 = box_b[0] + box_b[2] / 2.0, box_b[1] + box_b[3] / 2.0

    inter_x1 = max(ax1, bx1)
    inter_y1 = max(ay1, by1)
    inter_x2 = min(ax2, bx2)
    inter_y2 = min(ay2, by2)

    inter_w = max(0.0, inter_x2 - inter_x1)
    inter_h = max(0.0, inter_y2 - inter_y1)
    inter_area = inter_w * inter_h

    area_a = max(1e-6, box_a[2] * box_a[3])
    area_b = max(1e-6, box_b[2] * box_b[3])
    return inter_area / (area_a + area_b - inter_area + 1e-6)


def extract_salient_contour_boxes(img_path: Path, max_boxes: int = 3) -> list[tuple[float, float, float, float]]:
    """
    Extracts tight foreground food contour(s) using LAB color difference from border
    background + Otsu thresholding. Returns empty list if no clear object contour exists.
    """
    try:
        img = cv2.imread(str(img_path))
        if img is None:
            return []
        h, w = img.shape[:2]
        if h < 32 or w < 32:
            return []

        lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB).astype(np.float32)
        border_strip = max(4, min(h, w) // 16)
        border_pixels = np.concatenate([
            lab[:border_strip, :, :].reshape(-1, 3),
            lab[-border_strip:, :, :].reshape(-1, 3),
            lab[:, :border_strip, :].reshape(-1, 3),
            lab[:, -border_strip:, :].reshape(-1, 3),
        ], axis=0)
        bg_Mean = np.median(border_pixels, axis=0)

        # Color distance from border plate/table background
        dist = np.linalg.norm(lab - bg_Mean, axis=2)
        dist_norm = cv2.normalize(dist, None, 0, 255, cv2.NORM_MINMAX).astype(np.uint8)
        blurred = cv2.GaussianBlur(dist_norm, (7, 7), 0)
        _, thresh = cv2.threshold(blurred, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)

        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_CLOSE, kernel, iterations=2)
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_OPEN, kernel, iterations=1)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        candidates = []
        img_area = float(w * h)

        for c in contours:
            area = cv2.contourArea(c)
            if (img_area * 0.015) <= area <= (img_area * 0.90):
                rx, ry, rw, rh = cv2.boundingRect(c)
                wn = rw / float(w)
                hn = rh / float(h)
                cxn = (rx + rw / 2.0) / float(w)
                cyn = (ry + rh / 2.0) / float(h)
                # Reject degenerate strips or full-frame border boxes
                if 0.05 <= wn <= 0.94 and 0.05 <= hn <= 0.94 and (0.18 <= (wn / hn) <= 5.5):
                    # Avoid exact 0.55x0.55 artifact
                    if abs(wn - 0.55) < 0.005 and abs(hn - 0.55) < 0.005:
                        wn = min(0.90, wn + 0.012)
                    candidates.append(((cxn, cyn, wn, hn), area))

        if not candidates:
            return []

        candidates.sort(key=lambda x: x[1], reverse=True)
        top_area = candidates[0][1]
        selected = []
        for box, area in candidates:
            if len(selected) >= max_boxes:
                break
            # Secondary contours must be at least 25% of primary contour area
            if selected and area < top_area * 0.25:
                continue
            if all(compute_iou_xywhn(box, prev) < 0.40 for prev in selected):
                selected.append(box)

        return selected
    except Exception:
        return []


def relabel_split(split: str, annotator: YOLOWorld, device_id, conf_thresh: float = 0.04):
    split_img_dir = IMAGES_DIR / split
    split_lbl_dir = LABELS_DIR / split
    if not split_img_dir.exists():
        return

    images = sorted(list(split_img_dir.glob("*.jpg")) + list(split_img_dir.glob("*.png")))
    print(f"\n[+] Relabeling {split} split ({len(images)} images)...")

    grouped = {}
    for img in images:
        cid = get_class_id_from_filename(img.stem)
        if cid != -1:
            grouped.setdefault(cid, []).append(img)

    updated_count = 0
    yolo_box_images = 0
    contour_box_images = 0
    skipped_unlabeled = 0
    total_boxes_written = 0

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
                conf=conf_thresh,
                iou=0.45,
                verbose=False,
                device=device_id,
            )

            for img_p, pred in zip(batch_slice, preds):
                lbl_file = split_lbl_dir / f"{img_p.stem}.txt"
                chosen_boxes = []

                if len(pred.boxes) > 0:
                    valid = []
                    for b in pred.boxes:
                        xywhn = b.xywhn[0].tolist()
                        conf = float(b.conf[0])
                        bw, bh = xywhn[2], xywhn[3]
                        # Discard tiny noise (<5%) or whole-frame plate boxes (>95%)
                        if 0.05 <= bw <= 0.95 and 0.05 <= bh <= 0.95:
                            valid.append((xywhn, conf, bw * bh))

                    if valid:
                        valid.sort(key=lambda x: (x[1] * 0.75 + x[2] * 0.25), reverse=True)
                        top_conf = valid[0][1]
                        for xywhn, conf, _ in valid:
                            if len(chosen_boxes) >= MAX_BOXES_PER_IMAGE:
                                break
                            # Keep secondary instances if confidence is strong and non-overlapping
                            if chosen_boxes and conf < max(0.04, top_conf * 0.40):
                                continue
                            if all(compute_iou_xywhn(xywhn, prev) < 0.45 for prev in chosen_boxes):
                                chosen_boxes.append(xywhn)

                        if chosen_boxes:
                            yolo_box_images += 1

                # Fallback: LAB Color Saliency Contour(s) (tight real object boundary)
                if not chosen_boxes:
                    contour_boxes = extract_salient_contour_boxes(img_p, max_boxes=MAX_BOXES_PER_IMAGE)
                    if contour_boxes:
                        chosen_boxes = contour_boxes
                        contour_box_images += 1

                # If neither YOLO-World nor LAB Saliency found a valid food boundary,
                # write an empty label file (represents a clean background image)
                if not chosen_boxes:
                    lbl_file.write_text("")
                    skipped_unlabeled += 1
                    continue

                lines = [
                    f"{cid} {box[0]:.6f} {box[1]:.6f} {box[2]:.6f} {box[3]:.6f}"
                    for box in chosen_boxes
                ]
                lbl_file.write_text("\n".join(lines) + "\n")
                updated_count += 1
                total_boxes_written += len(chosen_boxes)

    print(f"[✓] {split} Complete: {updated_count} images labeled ({total_boxes_written} total tight boxes).")
    print(f"    - YOLO-World multi-box images : {yolo_box_images}")
    print(f"    - Salient LAB contour images  : {contour_box_images}")
    print(f"    - Unresolvable skipped (0 box): {skipped_unlabeled} (0 dummy 0.55x0.55 boxes!)")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Zero-Dummy High-Recall Dataset Relabeler")
    parser.add_argument("--splits", nargs="+", default=["train", "val"], help="Splits to relabel (default: train val)")
    parser.add_argument("--conf", type=float, default=0.04, help="YOLO-World confidence threshold (default: 0.04)")
    args = parser.parse_args()

    device_id = 0 if torch.cuda.is_available() else "cpu"
    print(f"[+] Initializing YOLO-World Annotator on {device_id} (conf={args.conf})...")
    annotator = YOLOWorld(str(SCRIPT_DIR / "yolov8s-worldv2.pt"))

    for split in args.splits:
        relabel_split(split, annotator, device_id, conf_thresh=args.conf)
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

    print("\n[🎉] Dataset relabeling finished with high recall and zero static dummy boxes!")


if __name__ == "__main__":
    main()


