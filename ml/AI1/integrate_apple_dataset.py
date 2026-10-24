#!/usr/bin/env python3
"""
BawalVision Apple Dataset Auto-Annotator & YOLO Integrator
=========================================================
Converts raw apple photographs into
annotated YOLOv8 format with bounding boxes via local YOLO-World (zero-shot).
- Generates Class ID 9 (apple) normalized bounding boxes.
- Performs stratified split: 80% train, 10% val, 10% test.
- Automatically updates ml/AI1/yolo_dataset/dataset.yaml to 10 classes.
"""

import argparse
import os
import random
import shutil
import sys
from pathlib import Path
from PIL import Image
import torch
import yaml
from ultralytics import YOLOWorld

# Ensure UTF-8 on Windows console
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def auto_annotate_and_integrate(
    raw_apple_dir: Path,
    yolo_dataset_dir: Path,
    train_ratio: float = 0.80,
    val_ratio: float = 0.10,
    seed: int = 42,
    conf_threshold: float = 0.15
) -> int:
    random.seed(seed)
    valid_exts = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
    raw_files = [f for f in raw_apple_dir.iterdir() if f.is_file() and f.suffix.lower() in valid_exts]

    if not raw_files:
        print(f"[!] No image files found in '{raw_apple_dir}'.")
        print(f"    Please place your 500 apple images into: {raw_apple_dir.resolve()}")
        return 0

    print(f"\n{'='*70}")
    print(f" [APPLE INTEGRATION] Found {len(raw_files)} apple images in {raw_apple_dir}")
    print(f"{'='*70}")

    random.shuffle(raw_files)
    n_total = len(raw_files)
    n_train = int(n_total * train_ratio)
    n_val = int(n_total * val_ratio)

    splits = {
        "train": raw_files[:n_train],
        "val": raw_files[n_train:n_train + n_val],
        "test": raw_files[n_train + n_val:]
    }

    print(f"Partition: {len(splits['train'])} Train | {len(splits['val'])} Val | {len(splits['test'])} Test")

    # Load local YOLO-World model for zero-shot bounding box prediction
    repo_root = Path(__file__).resolve().parent.parent.parent
    world_model_path = repo_root / "ml" / "AI1" / "yolov8s-worldv2.pt"

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"[+] Loading YOLO-World Auto-Annotator ({world_model_path.name}) on {device.upper()}...")
    world_model = YOLOWorld(str(world_model_path))
    world_model.set_classes(["apple", "red apple", "green apple", "fresh apple fruit", "fruit"])

    total_annotated = 0

    for split_name, file_list in splits.items():
        img_dest_dir = yolo_dataset_dir / "images" / split_name
        lbl_dest_dir = yolo_dataset_dir / "labels" / split_name
        img_dest_dir.mkdir(parents=True, exist_ok=True)
        lbl_dest_dir.mkdir(parents=True, exist_ok=True)

        for idx, src_file in enumerate(file_list, 1):
            file_id = f"apple_{idx:05d}"
            dest_img = img_dest_dir / f"{file_id}.jpg"
            dest_lbl = lbl_dest_dir / f"{file_id}.txt"

            try:
                # 1. Standardize image to RGB JPEG
                with Image.open(src_file) as img:
                    img_rgb = img.convert("RGB")
                    w, h = img_rgb.size
                    img_rgb.save(dest_img, "JPEG", quality=95)

                # 2. Run zero-shot detection with YOLO-World
                preds = world_model(str(dest_img), conf=conf_threshold, verbose=False, device=device)[0]

                boxes = []
                if len(preds.boxes) > 0:
                    for box in preds.boxes:
                        # xywhn: normalized [x_center, y_center, width, height]
                        x_c, y_c, bw, bh = box.xywhn[0].tolist()
                        boxes.append(f"9 {x_c:.6f} {y_c:.6f} {bw:.6f} {bh:.6f}")

                # 3. Fallback: If zero-shot didn't catch the box, use center-object framing
                # (Standard for Kaggle single-fruit classification photos)
                if not boxes:
                    boxes.append("9 0.500000 0.500000 0.800000 0.800000")

                # Write YOLO format label file (Class ID 9 = apple)
                dest_lbl.write_text("\n".join(boxes) + "\n", encoding="utf-8")
                total_annotated += 1

            except Exception as e:
                print(f"[WARN] Error annotating {src_file.name}: {e}")
                continue

        print(f"  [✓] Split '{split_name}': Processed {len(file_list)} images -> {img_dest_dir}")

    # Step 3: Update dataset.yaml to 10 classes
    yaml_path = yolo_dataset_dir / "dataset.yaml"
    update_dataset_yaml_to_10_classes(yaml_path)

    print(f"\n{'='*70}")
    print(f" [SUCCESS] Successfully integrated {total_annotated} Apple images into YOLO dataset!")
    print(f" Updated Dataset YAML: {yaml_path}")
    print(f"{'='*70}\n")
    return total_annotated


def update_dataset_yaml_to_10_classes(yaml_path: Path):
    """Updates dataset.yaml to define 10 classes with 'apple' at index 9."""
    if not yaml_path.exists():
        return

    content = {
        "path": str(yaml_path.parent.resolve()),
        "train": "images/train",
        "val": "images/val",
        "test": "images/test",
        "nc": 10,
        "names": {
            0: "banana",
            1: "chicken",
            2: "egg",
            3: "orange",
            4: "rice",
            5: "spinach",
            6: "tempeh",
            7: "tofu",
            8: "water_spinach",
            9: "apple"
        }
    }

    with open(yaml_path, "w", encoding="utf-8") as f:
        yaml.dump(content, f, sort_keys=False, default_flow_style=False)


def main():
    parser = argparse.ArgumentParser(description="Auto-Annotate & Integrate Apple Dataset to YOLOv8")
    parser.add_argument(
        "--source-dir", "-s",
        default="ml/dataset/apple",
        help="Path to folder containing raw apple images (default: ml/dataset/apple)"
    )
    parser.add_argument(
        "--output-dataset", "-o",
        default="ml/AI1/yolo_dataset",
        help="Path to YOLO dataset directory (default: ml/AI1/yolo_dataset)"
    )

    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parent.parent.parent
    source_dir = (repo_root / args.source_dir).resolve() if not Path(args.source_dir).is_absolute() else Path(args.source_dir)
    yolo_dir = (repo_root / args.output_dataset).resolve() if not Path(args.output_dataset).is_absolute() else Path(args.output_dataset)

    auto_annotate_and_integrate(source_dir, yolo_dir)


if __name__ == "__main__":
    main()

