import os
import shutil
from pathlib import Path
from typing import Dict, List
import yaml
from tqdm import tqdm
from ultralytics import YOLOWorld

# ==============================================================================
# DIRECTORY & CLASS TAXONOMY CONFIGURATION
# ==============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent
SOURCE_DATASET_DIR = SCRIPT_DIR / "dataset"
OUTPUT_YOLO_DIR = SCRIPT_DIR / "yolo_dataset"

# 9 target classes mapped alphabetically with zero-shot semantic prompts
CLASS_MAPPING: Dict[str, Dict[str, any]] = {
    "banana": {"id": 0, "prompt": "yellow ripe banana fruit"},
    "chicken": {"id": 1, "prompt": "fried chicken or cooked meat portion"},
    "egg": {"id": 2, "prompt": "boiled or fried egg"},
    "orange": {"id": 3, "prompt": "orange citrus fruit"},
    "rice": {"id": 4, "prompt": "cooked white rice mound"},
    "spinach": {"id": 5, "prompt": "cooked green spinach vegetable"},
    "tempeh": {"id": 6, "prompt": "fried tempeh slice"},
    "tofu": {"id": 7, "prompt": "fried tofu or tofu cubes"},
    "water_spinach": {"id": 8, "prompt": "stir fried water spinach greens"},
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


def main() -> None:
    initialize_yolo_layout()

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
            prompt = meta["prompt"]

            # Narrow model vocabulary strictly to the current category
            annotator.set_classes([prompt])

            images: List[Path] = [
                img
                for img in class_folder.iterdir()
                if img.suffix.lower() in VALID_IMAGE_EXTS
            ]

            print(
                f" -> Class [{folder_name}] (ID: {class_id}) | Found: {len(images)} images"
            )

            for img_path in tqdm(
                images, desc=f"  Generating labels for {folder_name}", unit="img"
            ):
                dest_image = OUTPUT_YOLO_DIR / "images" / split / img_path.name
                dest_label = (
                    OUTPUT_YOLO_DIR
                    / "labels"
                    / split
                    / f"{img_path.stem}.txt"
                )

                if not dest_image.exists():
                    shutil.copy2(img_path, dest_image)

                # Execute bounding box inference (GPU Device 0 if available)
                predictions = annotator.predict(
                    str(dest_image), conf=0.15, verbose=False, device=0
                )
                detected_boxes = predictions[0].boxes

                label_rows = []
                if len(detected_boxes) > 0:
                    for box in detected_boxes:
                        # Normalized center x, center y, width, height: [0.0 - 1.0]
                        xywhn = box.xywhn[0].tolist()
                        label_rows.append(
                            f"{class_id} {xywhn[0]:.6f} {xywhn[1]:.6f} {xywhn[2]:.6f} {xywhn[3]:.6f}\n"
                        )
                else:
                    # Fallback: Assign default central bounding box (85% coverage)
                    label_rows.append(
                        f"{class_id} 0.500000 0.500000 0.850000 0.850000\n"
                    )

                with open(dest_label, "w", encoding="utf-8") as lf:
                    lf.writelines(label_rows)

    build_dataset_yaml()
    print(
        "\n[✓] Dataset conversion complete. Output available at ml/AI1/yolo_dataset/"
    )


if __name__ == "__main__":
    main()