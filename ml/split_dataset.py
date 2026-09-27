#!/usr/bin/env python3
"""
BawalVision Leak-Free Dataset Splitter
=====================================
Partitions raw image datasets into Train (70%), Validation (15%), and Test (15%)
with built-in Perceptual dHash & MD5 deduplication to guarantee zero Data Leakage.
"""

import argparse
import hashlib
import os
import random
import shutil
import subprocess
import sys
from pathlib import Path
from PIL import Image


def calculate_dhash(image: Image.Image, hash_size: int = 8) -> int:
    """Computes a 64-bit difference hash (dHash) for visual gradient comparison."""
    resized = image.convert("L").resize((hash_size + 1, hash_size), Image.Resampling.LANCZOS)
    pixels = list(resized.getdata())
    difference = []
    for row in range(hash_size):
        row_offset = row * (hash_size + 1)
        for col in range(hash_size):
            difference.append(pixels[row_offset + col] > pixels[row_offset + col + 1])
    hash_val = 0
    for idx, bit in enumerate(difference):
        if bit:
            hash_val |= (1 << idx)
    return hash_val


def hamming_distance(h1: int, h2: int) -> int:
    """Calculates differing bits between two 64-bit hashes."""
    return bin(h1 ^ h2).count("1")


def calculate_md5(file_path: str) -> str:
    """Computes binary MD5 hash."""
    hasher = hashlib.md5()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def collect_clean_images(category_dir: str, min_size: tuple, hamming_threshold: int = 4):
    """Scans, validates with PIL, and deduplicates images within a category."""
    valid_exts = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
    min_w, min_h = min_size
    
    seen_md5 = set()
    seen_dhashes = []
    clean_files = []

    files = sorted(os.listdir(category_dir))
    for file in files:
        ext = os.path.splitext(file)[1].lower()
        if ext not in valid_exts:
            continue
        
        file_path = os.path.join(category_dir, file)
        if not os.path.isfile(file_path) or os.path.getsize(file_path) == 0:
            continue

        # Verify integrity & format
        try:
            with Image.open(file_path) as img:
                img.verify()
            
            with Image.open(file_path) as img:
                img_rgb = img.convert("RGB")
                w, h = img_rgb.size
                if w < min_w or h < min_h:
                    continue

                # MD5 deduplication
                md5_val = calculate_md5(file_path)
                if md5_val in seen_md5:
                    continue

                # Perceptual dHash deduplication (near-duplicates)
                img_dhash = calculate_dhash(img_rgb)
                is_near_dup = False
                for existing_hash in seen_dhashes:
                    if hamming_distance(img_dhash, existing_hash) <= hamming_threshold:
                        is_near_dup = True
                        break

                if is_near_dup:
                    continue

                seen_md5.add(md5_val)
                seen_dhashes.append(img_dhash)
                clean_files.append(file_path)
        except Exception:
            continue

    return clean_files


def split_dataset(source_dir: str, output_dir: str, train_ratio: float = 0.70,
                  val_ratio: float = 0.15, test_ratio: float = 0.15,
                  min_size: tuple = (200, 200), seed: int = 42):
    """
    Splits images from source_dir into output_dir/train, output_dir/val, output_dir/test.
    """
    random.seed(seed)
    
    if not os.path.exists(source_dir):
        print(f"[ERROR] Source dataset directory '{source_dir}' does not exist.")
        sys.exit(1)

    print(f"\n{'='*70}")
    print(f" [DATASET SPLITTER] Anti-Leak Stratified Partition")
    print(f" Source: {source_dir}")
    print(f" Output: {output_dir}")
    print(f" Ratios: {train_ratio*100:.0f}% Train | {val_ratio*100:.0f}% Val | {test_ratio*100:.0f}% Test")
    print(f"{'='*70}\n")

    # Discover categories
    categories = [d for d in sorted(os.listdir(source_dir)) if os.path.isdir(os.path.join(source_dir, d))]
    
    if not categories:
        print(f"[ERROR] No category folders found inside '{source_dir}'.")
        sys.exit(1)

    # Initialize destination directories
    for subset in ["train", "val", "test"]:
        for cat in categories:
            os.makedirs(os.path.join(output_dir, subset, cat), exist_ok=True)

    summary_stats = {"train": 0, "val": 0, "test": 0, "total_raw": 0, "total_clean": 0}

    for cat in categories:
        cat_src = os.path.join(source_dir, cat)
        all_raw_files = [f for f in os.listdir(cat_src) if os.path.isfile(os.path.join(cat_src, f))]
        summary_stats["total_raw"] += len(all_raw_files)

        # Sanitize and deduplicate
        clean_files = collect_clean_images(cat_src, min_size=min_size)
        summary_stats["total_clean"] += len(clean_files)

        random.shuffle(clean_files)
        n_clean = len(clean_files)
        
        n_train = int(n_clean * train_ratio)
        n_val = int(n_clean * val_ratio)
        
        train_list = clean_files[:n_train]
        val_list = clean_files[n_train:n_train + n_val]
        test_list = clean_files[n_train + n_val:]

        # Copy images with standardized names
        for subset, file_list in [("train", train_list), ("val", val_list), ("test", test_list)]:
            dest_dir = os.path.join(output_dir, subset, cat)
            for idx, src_path in enumerate(file_list, 1):
                ext = os.path.splitext(src_path)[1].lower()
                dest_file = os.path.join(dest_dir, f"{cat}_{idx:05d}{ext}")
                shutil.copy2(src_path, dest_file)
            summary_stats[subset] += len(file_list)

        dropped = len(all_raw_files) - n_clean
        print(f"  * {cat:18s}: Clean={n_clean:4d} (Dropped={dropped:3d}) -> Train={len(train_list):4d} | Val={len(val_list):4d} | Test={len(test_list):4d}")

    print(f"\n{'='*70}")
    print(f" [SPLIT SUMMARY]")
    print(f"  Total Raw Scanned : {summary_stats['total_raw']:5d}")
    print(f"  Total Clean Saved : {summary_stats['total_clean']:5d}")
    print(f"  -> Train Set      : {summary_stats['train']:5d} images ({summary_stats['train']/max(1, summary_stats['total_clean'])*100:.1f}%)")
    print(f"  -> Validation Set : {summary_stats['val']:5d} images ({summary_stats['val']/max(1, summary_stats['total_clean'])*100:.1f}%)")
    print(f"  -> Test Set       : {summary_stats['test']:5d} images ({summary_stats['test']/max(1, summary_stats['total_clean'])*100:.1f}%)")
    print(f"{'='*70}")


def track_with_dvc(output_dir: str):
    """Tracks the output split directory with DVC."""
    dvc_bin = shutil.which("dvc") or os.path.expanduser("~/.local/bin/dvc")
    try:
        cmd = [dvc_bin, "add", output_dir]
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        dvc_file = f"{output_dir}.dvc"
        print(f"\n[SUCCESS] Split dataset tracked in DVC: {dvc_file}")
        print(f"[HINT] Run: git add {dvc_file} .gitignore && git commit -m \"chore(data): track train/val/test split\"")
    except Exception as e:
        print(f"[NOTE] DVC tracking manual command: dvc add {output_dir}")


def main():
    parser = argparse.ArgumentParser(
        description="BawalVision Leak-Free Dataset Splitter (Train / Val / Test)"
    )
    parser.add_argument(
        "--source-dir", "-s",
        default="dataset_mbg_matang",
        help="Source directory containing category folders of images"
    )
    parser.add_argument(
        "--output-dir", "-o",
        default="ml/AI1/dataset",
        help="Destination directory to output train/val/test folders (default: ml/AI1/dataset)"
    )
    parser.add_argument(
        "--train-ratio",
        type=float,
        default=0.70,
        help="Proportion for Training set (default: 0.70)"
    )
    parser.add_argument(
        "--val-ratio",
        type=float,
        default=0.15,
        help="Proportion for Validation set (default: 0.15)"
    )
    parser.add_argument(
        "--test-ratio",
        type=float,
        default=0.15,
        help="Proportion for Test set (default: 0.15)"
    )
    parser.add_argument(
        "--dvc-track",
        action="store_true",
        default=True,
        help="Automatically track split directory with DVC (default: True)"
    )

    args = parser.parse_args()

    split_dataset(
        source_dir=args.source_dir,
        output_dir=args.output_dir,
        train_ratio=args.train_ratio,
        val_ratio=args.val_ratio,
        test_ratio=args.test_ratio
    )

    if args.dvc_track:
        track_with_dvc(args.output_dir)


if __name__ == "__main__":
    main()
