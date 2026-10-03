#!/usr/bin/env python3
"""
BawalVision Safe Parallel Data Augmentation Engine (Train Set Only)
===================================================================
Applies domain-specific image transformations EXCLUSIVELY to the Train split.
Validation and Test sets are NEVER modified to prevent Data Leakage.
Accelerated using multi-core parallel thread/process workers.
"""

import argparse
import concurrent.futures
import os
import random
import sys
from pathlib import Path
from PIL import Image, ImageEnhance, ImageOps

# Configure UTF-8 for Windows consoles
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def transform_greenery(img: Image.Image) -> Image.Image:
    """Focuses on rotations & mild color jitter for leafy vegetables."""
    angle = random.uniform(-45, 45)
    img = img.rotate(angle, resample=Image.Resampling.BILINEAR, expand=False)
    if random.random() > 0.5:
        img = ImageOps.mirror(img)
    enhancer_b = ImageEnhance.Brightness(img)
    img = enhancer_b.enhance(random.uniform(0.9, 1.1))
    enhancer_c = ImageEnhance.Contrast(img)
    img = enhancer_c.enhance(random.uniform(0.85, 1.15))
    return img


def transform_protein(img: Image.Image) -> Image.Image:
    """Focuses on lighting reflections and orientation for meats, tofu & tempeh."""
    angle = random.uniform(-30, 30)
    img = img.rotate(angle, resample=Image.Resampling.BILINEAR, expand=False)
    if random.random() > 0.5:
        img = ImageOps.mirror(img)
    if random.random() > 0.8:
        img = ImageOps.flip(img)
    enhancer_b = ImageEnhance.Brightness(img)
    img = enhancer_b.enhance(random.uniform(0.8, 1.25))
    enhancer_c = ImageEnhance.Contrast(img)
    img = enhancer_c.enhance(random.uniform(0.8, 1.25))
    enhancer_s = ImageEnhance.Color(img)
    img = enhancer_s.enhance(random.uniform(0.85, 1.2))
    return img


def transform_fish(img: Image.Image) -> Image.Image:
    """Geometric orientations and reflection for pomfret/freshwater fish."""
    angle = random.uniform(-25, 25)
    img = img.rotate(angle, resample=Image.Resampling.BILINEAR, expand=False)
    if random.random() > 0.5:
        img = ImageOps.mirror(img)
    enhancer_b = ImageEnhance.Brightness(img)
    img = enhancer_b.enhance(random.uniform(0.85, 1.15))
    enhancer_c = ImageEnhance.Contrast(img)
    img = enhancer_c.enhance(random.uniform(0.85, 1.2))
    return img


def transform_standard(img: Image.Image) -> Image.Image:
    """Standard transformations for fruits and rice portions."""
    angle = random.uniform(-20, 20)
    img = img.rotate(angle, resample=Image.Resampling.BILINEAR, expand=False)
    if random.random() > 0.5:
        img = ImageOps.mirror(img)
    enhancer_b = ImageEnhance.Brightness(img)
    img = enhancer_b.enhance(random.uniform(0.9, 1.15))
    enhancer_c = ImageEnhance.Contrast(img)
    img = enhancer_c.enhance(random.uniform(0.9, 1.15))
    return img


CATEGORY_TRANSFORMS = {
    "spinach": transform_greenery,
    "bayam": transform_greenery,
    "water_spinach": transform_greenery,
    "kangkong": transform_greenery,
    "kangkung": transform_greenery,
    "chicken": transform_protein,
    "ayam": transform_protein,
    "tofu": transform_protein,
    "tahu": transform_protein,
    "tempeh": transform_protein,
    "tempe": transform_protein,
    "egg": transform_protein,
    "telur": transform_protein,
    "pomfret_fish": transform_fish,
    "bawal": transform_fish,
    "rice": transform_standard,
    "nasi": transform_standard,
    "orange": transform_standard,
    "jeruk": transform_standard,
    "banana": transform_standard,
    "pisang": transform_standard,
}


def _augment_single_task(args):
    """Worker task to generate a single augmented variation."""
    img_path, save_path, pipeline_name = args
    pipeline = CATEGORY_TRANSFORMS.get(pipeline_name.lower(), transform_standard)
    try:
        with Image.open(img_path) as img:
            img_rgb = img.convert("RGB")
            aug_img = pipeline(img_rgb)
            aug_img.save(save_path, "JPEG", quality=92)
        return True
    except Exception as e:
        print(f"[!] Augmentation failed for {img_path}: {e}")
        return False


def augment_category_folder(class_dir: str, class_name: str, target_count: int, num_workers: int = 4):
    """Augments images within a specific train class directory in parallel up to target_count."""
    if not os.path.exists(class_dir):
        return

    valid_exts = ('.jpg', '.jpeg', '.png', '.webp')
    original_images = [f for f in os.listdir(class_dir) if f.lower().endswith(valid_exts) and not f.startswith("aug_")]
    total_current = len([f for f in os.listdir(class_dir) if f.lower().endswith(valid_exts)])

    if total_current >= target_count or len(original_images) == 0:
        print(f" [=] '{class_name}': Sufficient images ({total_current}/{target_count}). Skipping augmentation.")
        return

    needed = target_count - total_current
    print(f" [+] '{class_name}': Current {total_current} images. Generating +{needed} augmented variations with {num_workers} parallel workers...")

    tasks = []
    for i in range(needed):
        rand_img_name = random.choice(original_images)
        img_path = os.path.join(class_dir, rand_img_name)
        base_name = os.path.splitext(rand_img_name)[0]
        save_path = os.path.join(class_dir, f"aug_{total_current + i}_{base_name}.jpg")
        tasks.append((img_path, save_path, class_name))

    generated = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=num_workers) as executor:
        results = executor.map(_augment_single_task, tasks)
        generated = sum(1 for r in results if r)

    print(f"  -> Generated {generated} new augmented images for '{class_name}'. Total: {total_current + generated}")


def main():
    default_workers = max(1, os.cpu_count() or 4)
    parser = argparse.ArgumentParser(
        description="BawalVision Safe Parallel Data Augmentation Engine (Train Split Only)"
    )
    parser.add_argument(
        "--train-dir", "-t",
        default="ml/dataset/split/train",
        help="Path to the training split directory (default: ml/dataset/split/train)"
    )
    parser.add_argument(
        "--target-count", "-n",
        type=int,
        default=500,
        help="Target minimum number of images per class in train set (default: 500)"
    )
    parser.add_argument(
        "--workers", "-w",
        type=int,
        default=default_workers,
        help=f"Number of parallel worker threads (default: {default_workers})"
    )

    args = parser.parse_args()

    # Safety Guard: Refuse to run on val or test directories
    norm_path = os.path.normpath(args.train_dir).lower()
    if "val" in norm_path or "test" in norm_path:
        print(f"\n[FATAL ERROR] Data Leakage Guard Triggered!")
        print(f"Refusing to augment '{args.train_dir}'. Augmentation MUST strictly only be applied to TRAIN set.")
        sys.exit(1)

    if not os.path.exists(args.train_dir):
        alt_path = "ml/AI1/dataset/train"
        if os.path.exists(alt_path):
            args.train_dir = alt_path
        else:
            print(f"[ERROR] Training directory not found at '{args.train_dir}'. Run dataset scraper first.")
            sys.exit(1)

    print(f"\n{'='*70}")
    print(f" [AUGMENTATION] Safe Parallel Training Data Expansion (Target: {args.target_count}/class | Workers: {args.workers})")
    print(f" Directory: {args.train_dir}")
    print(f"{'='*70}")

    subdirs = [d for d in os.listdir(args.train_dir) if os.path.isdir(os.path.join(args.train_dir, d))]
    for class_name in sorted(subdirs):
        class_dir = os.path.join(args.train_dir, class_name)
        augment_category_folder(class_dir, class_name, args.target_count, args.workers)

    print(f"\n[SUCCESS] Parallel train-only augmentation complete. Val/Test sets remain 100% untouched.\n")


if __name__ == "__main__":
    main()