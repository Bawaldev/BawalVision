import argparse
import concurrent.futures
import os
import shutil
from pathlib import Path
from typing import Dict, List
import torch
import yaml
try:
    from tqdm import tqdm
except ImportError:
    tqdm = lambda x, **kwargs: x
from ultralytics import YOLOWorld

# ==============================================================================
# DIRECTORY & CLASS TAXONOMY CONFIGURATION
# ==============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent
SOURCE_DATASET_DIR = SCRIPT_DIR / "dataset"
OUTPUT_YOLO_DIR = SCRIPT_DIR / "yolo_dataset"

# 9 target classes mapped alphabetically with multi-synonym semantic prompts
CLASS_MAPPING: Dict[str, Dict[str, any]] = {
    "banana": {
        "id": 0,
        "prompts": ["banana", "yellow banana", "banana fruit", "fruit"],
    },
    "chicken": {
        "id": 1,
        "prompts": ["fried chicken", "cooked chicken", "chicken meat", "poultry", "meat dish", "food"],
    },
    "egg": {
        "id": 2,
        "prompts": ["boiled egg", "fried egg", "egg", "sliced egg", "food"],
    },
    "orange": {
        "id": 3,
        "prompts": ["orange fruit", "sliced orange", "orange", "fruit"],
    },
    "rice": {
        "id": 4,
        "prompts": ["white rice", "cooked rice", "rice", "bowl of rice", "food"],
    },
    "spinach": {
        "id": 5,
        "prompts": ["cooked spinach", "green spinach", "spinach vegetable", "leafy greens", "vegetable dish", "food"],
    },
    "tempeh": {
        "id": 6,
        "prompts": ["tempeh", "fried tempeh", "tempeh slice", "soybean cake", "fried food", "food"],
    },
    "tofu": {
        "id": 7,
        "prompts": ["tofu", "fried tofu", "tofu cube", "white tofu", "fried food", "food"],
    },
    "water_spinach": {
        "id": 8,
        "prompts": ["water spinach", "stir fried water spinach", "kangkung", "green vegetable", "vegetable dish", "food"],
    },
}

VALID_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}


def initialize_yolo_layout() -> None:
    """Create directory structure for YOLO object detection."""
    for split in ["train", "val", "test"]:
        (OUTPUT_YOLO_DIR / "images" / split).mkdir(parents=True, exist_ok=True)
        (OUTPUT_YOLO_DIR / "labels" / split).mkdir(parents=True, exist_ok=True)


def build_dataset_yaml() -> Path:
    """Generate YOLO dataset metadata specification."""
    names_dict = {
        meta["id"]: class_name for class_name, meta in CLASS_MAPPING.items()
    }
    yaml_config = {
        "path": str(OUTPUT_YOLO_DIR.resolve()),
        "train": "images/train",
        "val": "images/val",
        "test": "images/test",
        "nc": len(names_dict),
        "names": names_dict,
    }
    yaml_target = OUTPUT_YOLO_DIR / "dataset.yaml"
    with open(yaml_target, "w", encoding="utf-8") as f:
        yaml.dump(yaml_config, f, sort_keys=False)
    print(f"[✓] YOLO specification file generated: {yaml_target}")
    return yaml_target


def _copy_task(args: tuple):
    src, dest = args
    if not dest.exists():
        shutil.copy2(src, dest)


def process_class_batch(annotator: YOLOWorld, images: List[Path], split: str,
                        folder_name: str, class_id: int, prompts: List[str],
                        batch_size: int, device_id: any, num_workers: int):
    """Processes a category's images using parallel copy & batched YOLO-World inference."""
    annotator.set_classes(prompts)

    # 1. Parallel copy to YOLO images folder
    dest_images = [OUTPUT_YOLO_DIR / "images" / split / img.name for img in images]
    copy_tasks = [(src, dst) for src, dst in zip(images, dest_images)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=num_workers) as executor:
        list(executor.map(_copy_task, copy_tasks))

    # 2. Batched zero-shot bounding box inference with IoU NMS and strict quality filtering
    for i in tqdm(range(0, len(dest_images), batch_size),
                 desc=f"  Generating labels for {folder_name}", unit="batch"):
        batch_slice = dest_images[i:i + batch_size]
        batch_paths_str = [str(p) for p in batch_slice]

        predictions = annotator.predict(
            batch_paths_str,
            batch=len(batch_paths_str),
            conf=0.28,
            iou=0.50,
            verbose=False,
            device=device_id
        )

        for dest_img, pred in zip(batch_slice, predictions):
            dest_label = OUTPUT_YOLO_DIR / "labels" / split / f"{dest_img.stem}.txt"
            detected_boxes = pred.boxes
            label_rows = []

            if len(detected_boxes) > 0:
                valid_boxes = []
                for box in detected_boxes:
                    xywhn = box.xywhn[0].tolist()
                    conf = float(box.conf[0])
                    bw, bh = xywhn[2], xywhn[3]

                    # QUALITY FILTER:
                    # 1. Confidence >= 0.35
                    # 2. Bounding box < 95% frame (reject entire-frame engulfment)
                    # 3. Bounding box > 5% frame (reject tiny noise artifacts)
                    if conf >= 0.35 and 0.05 < bw < 0.95 and 0.05 < bh < 0.95:
                        valid_boxes.append((xywhn, conf, bw * bh))

                # If multiple valid boxes detected, select the largest primary object
                if valid_boxes:
                    valid_boxes.sort(key=lambda x: x[2], reverse=True)
                    best = valid_boxes[0]
                    label_rows.append(
                        f"{class_id} {best[0][0]:.6f} {best[0][1]:.6f} {best[0][2]:.6f} {best[0][3]:.6f}\n"
                    )

            with open(dest_label, "w", encoding="utf-8") as lf:
                lf.writelines(label_rows)


def main() -> None:
    try:
        from ml.cpu_utils import get_optimal_workers
    except ImportError:
        try:
            from cpu_utils import get_optimal_workers
        except ImportError:
            def get_optimal_workers(reserve_cores=0):
                return max(1, (hasattr(os, "sched_getaffinity") and len(os.sched_getaffinity(0))) or os.cpu_count() or 4)

    default_workers = get_optimal_workers()
    parser = argparse.ArgumentParser(
        description="BawalVision Parallel Zero-Shot YOLO Dataset Generator"
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=16,
        help="Inference batch size for YOLO-World (default: 16)"
    )
    parser.add_argument(
        "--workers", "-w",
        type=int,
        default=0,
        help=f"Number of parallel file I/O workers (0 = auto-detect max threads: {default_workers})"
    )

    args = parser.parse_args()
    if args.workers <= 0:
        args.workers = default_workers

    initialize_yolo_layout()

    device_id = 0 if torch.cuda.is_available() else "cpu"
    device_name = "NVIDIA CUDA (GPU:0)" if device_id == 0 else "Host CPU"
    print(f"[+] Compute Hardware : {device_name}")
    print(f"[+] Parallel Workers : {args.workers} threads | Batch Size: {args.batch_size}")
    print("[+] Initializing Zero-Shot Foundation Model: yolov8s-worldv2.pt...")
    annotator = YOLOWorld("yolov8s-worldv2.pt")

    for split in ["train", "val", "test"]:
        split_src = SOURCE_DATASET_DIR / split
        if not split_src.exists():
            continue

        print(f"\n=== Processing Split: {split.upper()} ===")

        for folder_name, meta in CLASS_MAPPING.items():
            class_folder = split_src / folder_name
            if not class_folder.exists():
                continue

            class_id = meta["id"]
            prompts = meta["prompts"]

            images: List[Path] = [
                img for img in class_folder.iterdir()
                if img.suffix.lower() in VALID_IMAGE_EXTS
            ]

            if not images:
                continue

            print(
                f" -> Class [{folder_name}] (ID: {class_id}) | Found: {len(images)} images"
            )

            process_class_batch(
                annotator=annotator,
                images=images,
                split=split,
                folder_name=folder_name,
                class_id=class_id,
                prompts=prompts,
                batch_size=args.batch_size,
                device_id=device_id,
                num_workers=args.workers
            )

    build_dataset_yaml()
    print(
        "\n[✓] Parallel dataset conversion complete. Output available at ml/AI1/yolo_dataset/"
    )


if __name__ == "__main__":
    main()