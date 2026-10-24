#!/usr/bin/env python3
"""
BawalVision Pure Plate Hard-Negative Scraper ('piring' only)
============================================================
Direct 1-to-1 adaptation of ml/scrap/scraps.py targeting clean tableware ('piring').

Features (identical to scraps.py):
1. Multi-Engine Crawling via icrawler (Bing + Google + Baidu fallback).
2. Perceptual Hashing (dHash) + MD5 Deduplication: Detects and eliminates
   both exact binary duplicates and near-duplicates (resized, watermarked, recompressed).
3. Resolution & Format verification via Pillow (RGB JPEG, >= 200x200 px).
4. Auto-Injection of 0-byte negative labels into ml/AI1/yolo_dataset/labels/train/
   for Ultralytics YOLO background samples.
"""

import argparse
import hashlib
import os
import random
import shutil
import sys
import time
from pathlib import Path
from PIL import Image

try:
    from icrawler.builtin import BingImageCrawler, GoogleImageCrawler, BaiduImageCrawler
except ImportError:
    print("[ERROR] 'icrawler' is not installed. Run: pip install icrawler Pillow")
    sys.exit(1)

# Configure UTF-8 on Windows console
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Target search queries for clean, empty plates
LARGE_DATASET_QUERIES = {
    "piring": [
        # Piring makan ceper, keramik, melamin bersih di meja makan
        "piring makan kosong keramik",
        "piring putih ceper kosong meja",
        "piring makan melamin kosong",
        "empty white ceramic dinner plate top view",
        "clean empty plate on dining table",
        "piring keramik kosong di atas meja makan",
        "piring makan ceper kosong bersih",
        "empty round dinner plate overhead view",
        "piring makan kaca ceper kosong",
        "clean empty white porcelain plate table top down",
        "piring melamin putih ceper kosong",
        "empty white porcelain food plate table top down",
        "piring makan porselen putih polos bersih",
        "empty clean dining plate on restaurant table top down",
        "piring makan ceper keramik putih kosong",
        "clean empty restaurant food plate overhead view",
        "piring makan bulat ceper kosong meja makan",
        "empty white melamine dinner plate top view",
        "piring keramik bulat putih kosong bersih",
        "clean empty ceramic plate on wooden table top down",
        "piring makan kosong bersih top down",
        "empty white dish on table overhead",
        "piring ceper keramik putih polos",
        "clean empty plate overhead shot",
        "piring makan melamin putih bersih",
        "empty porcelain dinner dish top down",
        "piring makan ceper bersih di meja",
        "empty white ceramic plate wood background top view",
        "piring keramik saji ceper kosong",
        "empty clean dinner plate restaurant top view"
    ]
}


# =========================================================================
# Perceptual Hashing (dHash) & Deduplication Engine (exact match to scraps.py)
# =========================================================================

def calculate_dhash(image: Image.Image, hash_size: int = 8) -> int:
    """
    Computes a 64-bit difference hash (dHash) for perceptual visual similarity.
    Captures structural image gradient independent of resolution, watermark, or compression.
    """
    resized = image.convert("L").resize((hash_size + 1, hash_size), Image.Resampling.LANCZOS)
    pixels = list(resized.getdata())

    difference = []
    for row in range(hash_size):
        row_offset = row * (hash_size + 1)
        for col in range(hash_size):
            pixel_left = pixels[row_offset + col]
            pixel_right = pixels[row_offset + col + 1]
            difference.append(pixel_left > pixel_right)

    hash_val = 0
    for idx, bit in enumerate(difference):
        if bit:
            hash_val |= (1 << idx)
    return hash_val


def hamming_distance(h1: int, h2: int) -> int:
    """Calculates the number of differing bits between two 64-bit hashes."""
    return bin(h1 ^ h2).count("1")


def calculate_md5(file_path: str) -> str:
    """Computes exact binary MD5 hash."""
    hasher = hashlib.md5()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


class GlobalDeduplicator:
    """
    Tracks MD5 hashes and perceptual dHashes to guarantee zero duplicate
    or near-duplicate images.
    """
    def __init__(self, hamming_threshold: int = 4):
        self.seen_md5 = set()
        self.seen_dhashes = []  # List of (dhash_int, category, filepath)
        self.hamming_threshold = hamming_threshold

    def is_duplicate(self, file_path: str, category: str, img: Image.Image) -> bool:
        # Check 1: Exact MD5 match
        md5_val = calculate_md5(file_path)
        if md5_val in self.seen_md5:
            return True

        # Check 2: Perceptual dHash match (near-duplicate detection)
        img_dhash = calculate_dhash(img)
        for existing_hash, existing_cat, _ in self.seen_dhashes:
            dist = hamming_distance(img_dhash, existing_hash)
            if dist <= self.hamming_threshold:
                return True

        self.seen_md5.add(md5_val)
        self.seen_dhashes.append((img_dhash, category, file_path))
        return False


def validate_and_deduplicate(staging_dir: str, target_category_dir: str, category: str,
                             deduplicator: GlobalDeduplicator, min_size: tuple) -> int:
    """
    Validates images with PIL, eliminates exact and near-duplicates via dHash/MD5,
    and moves clean images into target category directory.
    """
    os.makedirs(target_category_dir, exist_ok=True)
    added_count = 0
    min_w, min_h = min_size
    valid_exts = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}

    for root, _, files in os.walk(staging_dir):
        for file in files:
            ext = os.path.splitext(file)[1].lower()
            if ext not in valid_exts:
                continue

            src_path = os.path.join(root, file)
            if not os.path.isfile(src_path) or os.path.getsize(src_path) == 0:
                continue

            # Step 1: Verify format & readable bytes
            try:
                with Image.open(src_path) as img_check:
                    img_check.verify()

                # Step 2: Open and check dimensions
                with Image.open(src_path) as img:
                    img_rgb = img.convert("RGB")
                    w, h = img_rgb.size
                    if w < min_w or h < min_h:
                        continue

                    # Step 3: Global perceptual & MD5 deduplication check
                    if deduplicator.is_duplicate(src_path, category, img_rgb):
                        continue

                    # Generate collision-free unique name
                    md5_prefix = calculate_md5(src_path)[:12]
                    dest_name = f"{category}_{md5_prefix}.jpg"
                    dest_path = os.path.join(target_category_dir, dest_name)

                    img_rgb.save(dest_path, "JPEG", quality=95)
                    added_count += 1
            except Exception:
                continue

    return added_count


def crawl_category(category: str, queries: list, target_dir: str, target_total: int,
                   engine: str, min_size: tuple, staging_root: str, deduplicator: GlobalDeduplicator,
                   threads: int = 8) -> int:
    """
    Crawls queries across selected search engines until target_total unique clean images are gathered.
    """
    category_dir = target_dir
    os.makedirs(category_dir, exist_ok=True)

    current_count = len([f for f in os.listdir(category_dir) if os.path.isfile(os.path.join(category_dir, f))])
    print(f"\n{'='*70}")
    print(f" [CATEGORY] '{category.upper()}' | Current Clean: {current_count} | Target: {target_total} | Workers: {threads}")
    print(f"{'='*70}")

    if current_count >= target_total:
        print(f"[INFO] Category '{category}' already reached target ({current_count}/{target_total}). Skipping.")
        return current_count

    staging_cat_dir = os.path.join(staging_root, category)

    engines = []
    if engine in ("bing", "all"):
        engines.append("bing")
    if engine in ("google", "all"):
        engines.append("google")
    if engine in ("baidu", "all"):
        engines.append("baidu")

    remaining = target_total - current_count
    num_queries = len(queries)
    max_per_query = max(20, min(100, (remaining // (num_queries * len(engines))) + 20))

    for term_idx, term in enumerate(queries, 1):
        if current_count >= target_total:
            print(f"[SUCCESS] Reached target image count ({current_count}/{target_total}) for '{category}'.")
            break

        for eng in engines:
            if current_count >= target_total:
                break

            if os.path.exists(staging_cat_dir):
                shutil.rmtree(staging_cat_dir, ignore_errors=True)
            os.makedirs(staging_cat_dir, exist_ok=True)

            print(f"\n[{term_idx}/{num_queries}] Engine: {eng.upper()} | Query: '{term}' (Targeting up to {max_per_query} imgs | Threads: {threads})")

            try:
                if eng == "bing":
                    crawler = BingImageCrawler(downloader_threads=threads, storage={"root_dir": staging_cat_dir}, log_level=30)
                elif eng == "google":
                    crawler = GoogleImageCrawler(downloader_threads=threads, storage={"root_dir": staging_cat_dir}, log_level=30)
                elif eng == "baidu":
                    crawler = BaiduImageCrawler(downloader_threads=threads, storage={"root_dir": staging_cat_dir}, log_level=30)
                else:
                    continue

                crawler.crawl(keyword=term, max_num=max_per_query, min_size=min_size)
            except Exception as e:
                print(f"[WARN] Crawler error on engine {eng} for query '{term}': {e}")

            # Validate, dHash deduplicate & merge
            newly_added = validate_and_deduplicate(staging_cat_dir, category_dir, category, deduplicator, min_size)
            current_count += newly_added
            print(f" -> Added: +{newly_added} clean unique images | Total in '{category}': {current_count}/{target_total}")
            time.sleep(random.uniform(0.5, 1.2))

    if os.path.exists(staging_cat_dir):
        shutil.rmtree(staging_cat_dir, ignore_errors=True)

    print(f"\n[DONE] Category '{category}' collected {current_count} verified non-duplicate images.")
    return current_count


# =========================================================================
# YOLO Hard Negative Sample Generator
# =========================================================================

def inject_yolo_hard_negatives(tray_dir: str, yolo_train_img_dir: str, yolo_train_lbl_dir: str) -> int:
    """
    Copies verified empty plates into YOLO train image directory and creates
    corresponding empty (0-byte) label files in YOLO train label directory.
    Ultralytics YOLO standard: 0-byte label file indicates pure background.
    """
    os.makedirs(yolo_train_img_dir, exist_ok=True)
    os.makedirs(yolo_train_lbl_dir, exist_ok=True)

    valid_exts = {".jpg", ".jpeg", ".png", ".webp"}
    images = [f for f in os.listdir(tray_dir) if os.path.isfile(os.path.join(tray_dir, f)) and os.path.splitext(f)[1].lower() in valid_exts]
    injected_count = 0

    for img_name in images:
        src_path = os.path.join(tray_dir, img_name)
        stem = os.path.splitext(img_name)[0]
        dest_img = os.path.join(yolo_train_img_dir, f"neg_{img_name}")
        dest_lbl = os.path.join(yolo_train_lbl_dir, f"neg_{stem}.txt")

        if not os.path.exists(dest_img):
            shutil.copy2(src_path, dest_img)
            with open(dest_lbl, "w") as f:
                pass  # empty 0-byte file
            injected_count += 1
        elif not os.path.exists(dest_lbl):
            with open(dest_lbl, "w") as f:
                pass

    print(f"\n[✓] Hard Negative Integration: Injected {injected_count} clean negative samples into:")
    print(f"    Images -> {yolo_train_img_dir}")
    print(f"    Labels -> {yolo_train_lbl_dir} (0-byte empty files)")
    return injected_count


# =========================================================================
# CLI Entrypoint
# =========================================================================

def main():
    parser = argparse.ArgumentParser(
        description="BawalVision Pure Plate Hard-Negative Scraper ('piring' only)"
    )
    parser.add_argument(
        "--category", "-c",
        default="piring",
        help="Category to scrape (default: 'piring')"
    )
    parser.add_argument(
        "--target", "--target-count", "-t", "-n",
        type=int,
        dest="target_count",
        default=1000,
        help="Target number of unique clean images (default: 1000)"
    )
    parser.add_argument(
        "--output-dir", "-o",
        default="ml/dataset/scraped_empty_trays",
        help="Directory to store clean downloaded dataset (default: ml/dataset/scraped_empty_trays)"
    )
    parser.add_argument(
        "--engine", "-e",
        choices=["bing", "google", "all"],
        default="all",
        help="Search engine(s) to use (default: all)"
    )
    parser.add_argument(
        "--min-width",
        type=int,
        default=200,
        help="Minimum image width in pixels (default: 200)"
    )
    parser.add_argument(
        "--min-height",
        type=int,
        default=200,
        help="Minimum image height in pixels (default: 200)"
    )
    parser.add_argument(
        "--staging-dir",
        default=".cache_trash/scraper_staging_piring",
        help="Temporary staging directory for downloads"
    )
    parser.add_argument(
        "--threads", "-w",
        type=int,
        default=8,
        help="Number of crawler download threads (default: 8)"
    )
    parser.add_argument(
        "--clean-existing",
        action="store_true",
        help="Purge existing images in output directory before scraping"
    )
    parser.add_argument(
        "--inject-negatives",
        action="store_true",
        default=True,
        help="Automatically copy scraped empty plates as 0-byte negative labels into yolo_dataset/train (default: True)"
    )

    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parent.parent.parent
    output_dir = str((repo_root / args.output_dir).resolve()) if not os.path.isabs(args.output_dir) else args.output_dir
    staging_dir = str((repo_root / args.staging_dir).resolve()) if not os.path.isabs(args.staging_dir) else args.staging_dir

    if args.clean_existing and os.path.exists(output_dir):
        print(f"[!] Purging existing files in '{output_dir}'...")
        shutil.rmtree(output_dir, ignore_errors=True)

    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(staging_dir, exist_ok=True)

    queries = LARGE_DATASET_QUERIES["piring"]
    deduplicator = GlobalDeduplicator(hamming_threshold=4)

    print(f"\n========================================================")
    print(f"  BawalVision Clean Plate Scraper (scraps.py Engine)")
    print(f"========================================================")
    print(f"Category: piring")
    print(f"Target Images: {args.target_count}")
    print(f"Engine Mode: {args.engine.upper()}")
    print(f"Crawler Threads: {args.threads}")
    print(f"Output Directory: {output_dir}")
    print(f"Anti-Leak Filter: Perceptual dHash (Hamming Dist <= 4) + MD5")
    print(f"Auto-Inject Negatives: {args.inject_negatives}")
    print(f"========================================================\n")

    total_collected = crawl_category(
        category="piring",
        queries=queries,
        target_dir=output_dir,
        target_total=args.target_count,
        engine=args.engine,
        min_size=(args.min_width, args.min_height),
        staging_root=staging_dir,
        deduplicator=deduplicator,
        threads=args.threads
    )

    # Cleanup staging root
    if os.path.exists(staging_dir):
        shutil.rmtree(staging_dir, ignore_errors=True)

    # Inject into YOLO dataset as 0-byte negatives
    if args.inject_negatives and os.path.exists(output_dir):
        yolo_img_dir = str(repo_root / "ml" / "AI1" / "yolo_dataset" / "images" / "train")
        yolo_lbl_dir = str(repo_root / "ml" / "AI1" / "yolo_dataset" / "labels" / "train")
        inject_yolo_hard_negatives(output_dir, yolo_img_dir, yolo_lbl_dir)

    print(f"\n[SUCCESS] Collected {total_collected} verified clean plates & integrated negatives into YOLO.\n")


if __name__ == "__main__":
    main()

    main()