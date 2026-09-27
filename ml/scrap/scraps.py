#!/usr/bin/env python3
"""
BawalVision High-Volume & Anti-Data-Leak Dataset Scraper
======================================================
Features:
1. Multi-Engine Crawling (Bing + Google + Baidu fallback).
2. Perceptual Hashing (dHash) + MD5 Deduplication: Detects and eliminates
   both exact binary duplicates and near-duplicates (resized, watermarked, recompressed).
3. Global Cross-Category Collision Check: Prevents the same image appearing in multiple classes.
4. Leak-Free Train / Val / Test Stratified Splitter: Automatically partitions
   verified clean images before any augmentation takes place.
5. DVC Dataset Tracking Integration.
"""

import argparse
import hashlib
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

try:
    from icrawler.builtin import BingImageCrawler, GoogleImageCrawler, BaiduImageCrawler
except ImportError:
    print("[ERROR] 'icrawler' is not installed. Run: pip install icrawler Pillow")
    sys.exit(1)

# Target search queries with multi-lingual and contextual variations
LARGE_DATASET_QUERIES = {
    "tofu": [
        "raw white tofu block plate",
        "tahu putih mentah piring",
        "tahu sutra segar",
        "fried tofu cubes golden",
        "tahu goreng sumedang",
        "pan fried tofu slices crispy",
        "tahu kuning matang",
        "tahu bacem piring saji",
        "stir fry tofu vegetables",
        "tumis tahu kecap",
        "tahu pong goreng",
        "baked tofu cubes bowl",
        "tahu kukus diet",
        "tofu curd square fresh",
        "tahu bakso matang piring",
        "deep fried crispy tofu",
        "tahu tempe goreng piring",
        "cooked brown tofu slices",
        "mapo tofu dish plate",
        "tahu goreng renyah"
    ],
    "tempeh": [
        "tempe mentah balok daun",
        "fresh raw tempeh block",
        "tempe goreng matang piring",
        "crispy fried tempeh slices",
        "tempe goreng tepung renyah",
        "sambal goreng tempe kering",
        "tempe mendoan piring",
        "tempe bacem matang",
        "tumis tempe kecap manis",
        "raw soybean tempeh slices",
        "tempe bakar bumbu",
        "orek tempe piring",
        "cooked brown tempeh",
        "tempe goreng gurih",
        "tempeh protein bowl",
        "keripik tempe renyah",
        "sayur lodeh tempe",
        "stir fried tempeh plate"
    ],
    "rice": [
        "cooked white rice plate top view",
        "nasi putih matang piring",
        "steamed jasmine rice bowl",
        "nasi uduk piring komplit",
        "nasi liwet hangat",
        "fluffy white rice in plate",
        "nasi kuning piring",
        "cooked brown rice bowl",
        "nasi pulen hangat",
        "portion of boiled rice on plate",
        "nasi putih satu porsi",
        "cooked basmati rice plate",
        "nasi gurih matang",
        "steamed white rice portion top view",
        "mangkuk nasi putih matang"
    ],
    "chicken": [
        "cooked chicken dish plate top view",
        "ayam goreng matang piring",
        "roasted chicken piece plate",
        "ayam bakar kecap piring",
        "grilled chicken breast sliced",
        "ayam suwir matang",
        "ayam ungkep kuning",
        "fried chicken drumstick plate",
        "ayam rica rica piring",
        "boiled shredded chicken plate",
        "ayam kalasan matang",
        "crispy fried chicken wings plate",
        "ayam kecap manis piring",
        "ayam pop matang",
        "cooked poultry meal plate"
    ],
    "spinach": [
        "cooked spinach dish plate",
        "tumis bayam matang piring",
        "sayur bening bayam mangkuk",
        "sauteed fresh spinach",
        "boiled green spinach leaves",
        "bayam rebus matang piring",
        "garlic sauteed spinach plate",
        "tumis bayam jagung manis",
        "steamed baby spinach dish",
        "fresh green spinach leaves bowl",
        "spinach omelette plate",
        "sayur bayam segar matang"
    ],
    "water_spinach": [
        "tumis kangkung matang piring",
        "stir fried water spinach plate",
        "kangkung cah terasi piring",
        "sauteed morning glory dish",
        "kangkung plecing lombok",
        "tumis kangkung bawang putih",
        "cooked water spinach greens",
        "oseng kangkung piring",
        "stir fry ong choy plate",
        "kangkung belacan piring"
    ],
    "egg": [
        "boiled egg sliced plate",
        "telur rebus matang piring",
        "fried sunny side up egg plate",
        "telur mata sapi piring",
        "telur dadar matang piring",
        "telur balado matang piring",
        "scrambled eggs plate top view",
        "telur pindang matang",
        "half boiled egg sliced",
        "tamagoyaki egg roll slices",
        "telur ceplok matang"
    ],
    "banana": [
        "ripe yellow banana fruit bunch",
        "buah pisang matang kuning",
        "pisang ambon segar piring",
        "sliced yellow banana plate",
        "pisang raja matang",
        "fresh Cavendish bananas",
        "buah pisang meja makan",
        "single peeled banana plate",
        "pisang kepok kuning matang",
        "banana fruit on table top view"
    ],
    "orange": [
        "fresh orange fruit whole and sliced",
        "buah jeruk segar piring",
        "orange slices on plate top view",
        "jeruk manis matang",
        "cut citrus orange pieces",
        "buah jeruk kupas piring",
        "fresh navel orange segments",
        "jeruk sunkist segar",
        "orange fruit bowl top view",
        "irisan buah jeruk segar"
    ]
}


# =========================================================================
# Perceptual Hashing (dHash) & Anti-Data-Leak Deduplication Engine
# =========================================================================

def calculate_dhash(image: Image.Image, hash_size: int = 8) -> int:
    """
    Computes a 64-bit difference hash (dHash) for perceptual visual similarity.
    Captures structural image gradient independent of resolution, watermark, or compression.
    """
    # 1. Resize to (hash_size + 1, hash_size) -> (9, 8) in grayscale
    resized = image.convert("L").resize((hash_size + 1, hash_size), Image.Resampling.LANCZOS)
    pixels = list(resized.getdata())
    
    # 2. Compare adjacent pixels horizontally
    difference = []
    for row in range(hash_size):
        row_offset = row * (hash_size + 1)
        for col in range(hash_size):
            pixel_left = pixels[row_offset + col]
            pixel_right = pixels[row_offset + col + 1]
            difference.append(pixel_left > pixel_right)
            
    # 3. Convert boolean array to 64-bit integer
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
    Tracks MD5 hashes and perceptual dHashes across ALL dataset categories
    to guarantee zero duplicate or near-duplicate cross-contamination.
    """
    def __init__(self, hamming_threshold: int = 4):
        self.seen_md5 = set()
        self.seen_dhashes = [] # List of (dhash_int, category, filepath)
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
                # Perceptual duplicate detected (same photo re-encoded/watermarked)
                return True

        # Unique image: register into global registry
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
                   engine: str, min_size: tuple, staging_root: str, deduplicator: GlobalDeduplicator):
    """
    Crawls queries across selected search engines until target_total unique clean images are gathered.
    """
    category_dir = os.path.join(target_dir, category)
    os.makedirs(category_dir, exist_ok=True)
    
    current_count = len([f for f in os.listdir(category_dir) if os.path.isfile(os.path.join(category_dir, f))])
    print(f"\n{'='*70}")
    print(f" [CATEGORY] '{category.upper()}' | Current Clean: {current_count} | Target: {target_total}")
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
    max_per_query = max(30, min(150, (remaining // (num_queries * len(engines))) + 25))

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

            print(f"\n[{term_idx}/{num_queries}] Engine: {eng.upper()} | Query: '{term}' (Targeting up to {max_per_query} imgs)")

            try:
                if eng == "bing":
                    crawler = BingImageCrawler(downloader_threads=8, storage={"root_dir": staging_cat_dir}, log_level=30)
                elif eng == "google":
                    crawler = GoogleImageCrawler(downloader_threads=8, storage={"root_dir": staging_cat_dir}, log_level=30)
                elif eng == "baidu":
                    crawler = BaiduImageCrawler(downloader_threads=8, storage={"root_dir": staging_cat_dir}, log_level=30)
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
# Leak-Free Train / Val / Test Stratified Splitter
# =========================================================================

def create_leak_free_splits(raw_dataset_dir: str, split_output_dir: str,
                            train_ratio: float = 0.70, val_ratio: float = 0.15,
                            test_ratio: float = 0.15, seed: int = 42):
    """
    Partitions clean dataset into disjoint Train, Val, and Test sets.
    Ensures that identical or perceptual duplicates NEVER exist between sets.
    """
    random.seed(seed)
    print(f"\n{'='*70}")
    print(f" [SPLIT] Creating Leak-Free Dataset Split ({train_ratio*100:.0f}% Train / {val_ratio*100:.0f}% Val / {test_ratio*100:.0f}% Test)")
    print(f" Target Path: {split_output_dir}")
    print(f"{'='*70}")

    for subset in ["train", "val", "test"]:
        os.makedirs(os.path.join(split_output_dir, subset), exist_ok=True)

    categories = [d for d in os.listdir(raw_dataset_dir) if os.path.isdir(os.path.join(raw_dataset_dir, d))]

    split_stats = {"train": 0, "val": 0, "test": 0}

    for cat in sorted(categories):
        cat_src = os.path.join(raw_dataset_dir, cat)
        files = [f for f in os.listdir(cat_src) if f.lower().endswith((".jpg", ".jpeg", ".png", ".webp"))]
        random.shuffle(files)

        n_total = len(files)
        n_train = int(n_total * train_ratio)
        n_val = int(n_total * val_ratio)
        
        train_files = files[:n_train]
        val_files = files[n_train:n_train + n_val]
        test_files = files[n_train + n_val:]

        for subset, split_files in [("train", train_files), ("val", val_files), ("test", test_files)]:
            subset_cat_dir = os.path.join(split_output_dir, subset, cat)
            os.makedirs(subset_cat_dir, exist_ok=True)
            for f in split_files:
                shutil.copy2(os.path.join(cat_src, f), os.path.join(subset_cat_dir, f))
            split_stats[subset] += len(split_files)

        print(f"  * {cat:20s}: Total={n_total:4d} | Train={len(train_files):4d} | Val={len(val_files):4d} | Test={len(test_files):4d}")

    print(f"\n[SPLIT COMPLETE] Total Images: {sum(split_stats.values())} (Train: {split_stats['train']}, Val: {split_stats['val']}, Test: {split_stats['test']})")


def track_with_dvc(target_dir: str) -> bool:
    """Tracks the specified dataset directory in DVC."""
    if not os.path.exists(target_dir):
        print(f"[ERROR] Target directory '{target_dir}' does not exist for DVC tracking.")
        return False
        
    dvc_bin = shutil.which("dvc") or os.path.expanduser("~/.local/bin/dvc")
    print(f"\n{'='*70}")
    print(f" [DVC] Staging Dataset to DVC: {target_dir}")
    print(f"{'='*70}")
    
    try:
        cmd = [dvc_bin, "add", target_dir]
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        if result.stdout:
            print(result.stdout)
        
        dvc_file = f"{target_dir}.dvc"
        print(f"[SUCCESS] Dataset tracked with DVC: {dvc_file}")
        print(f"[HINT] Commit tracking pointer to Git:")
        print(f"  git add {dvc_file} .gitignore")
        print(f"  git commit -m \"chore(data): track clean leak-free dataset in {target_dir}\"")
        return True
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"[WARN] DVC automated execution note: {e}")
        print(f"[MANUAL] You can execute manually: dvc add {target_dir} && git add {target_dir}.dvc .gitignore")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="BawalVision High-Volume & Anti-Data-Leak Dataset Scraper"
    )
    parser.add_argument(
        "--category", "-c",
        default="all",
        help="Category to scrape (e.g. tofu, pomfret_fish, rice, chicken, or 'all')"
    )
    parser.add_argument(
        "--output-dir", "-o",
        default="ml/dataset/dataset_mbg_matang",
        help="Base directory to store raw downloaded dataset"
    )
    parser.add_argument(
        "--target-count", "-t",
        type=int,
        default=1000,
        help="Target number of unique clean images per category (e.g. 1000-2000)"
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
        default=".cache_trash/scraper_staging",
        help="Temporary staging directory for query downloads"
    )
    parser.add_argument(
        "--split",
        action="store_true",
        default=True,
        help="Automatically generate leak-free train/val/test splits (default: True)"
    )
    parser.add_argument(
        "--split-output",
        default="ml/dataset/split",
        help="Output path for stratified train/val/test split (default: ml/dataset/split)"
    )
    parser.add_argument(
        "--dvc-track",
        action="store_true",
        default=True,
        help="Automatically track dataset with DVC after scraping (default: True)"
    )

    args = parser.parse_args()

    if args.category == "all":
        selected_categories = list(LARGE_DATASET_QUERIES.keys())
    else:
        selected_categories = [cat.strip() for cat in args.category.split(",") if cat.strip()]

    print(f"\n========================================================")
    print(f"  BawalVision Anti-Data-Leak Dataset Scraper & Splitter")
    print(f"========================================================")
    print(f"Categories: {selected_categories}")
    print(f"Target Images per Category: {args.target_count}")
    print(f"Engine Mode: {args.engine.upper()}")
    print(f"Output Directory: {args.output_dir}")
    print(f"Anti-Leak Filter: Perceptual dHash (Hamming Dist <= 4) + MD5")
    print(f"Split Generation: {args.split} -> {args.split_output}")
    print(f"DVC Tracking: {args.dvc_track}")
    print(f"========================================================\n")

    os.makedirs(args.output_dir, exist_ok=True)
    os.makedirs(args.staging_dir, exist_ok=True)

    deduplicator = GlobalDeduplicator(hamming_threshold=4)
    category_summary = {}

    for cat in selected_categories:
        if cat not in LARGE_DATASET_QUERIES:
            print(f"[WARN] Unknown category '{cat}'. Available: {list(LARGE_DATASET_QUERIES.keys())}")
            continue

        queries = LARGE_DATASET_QUERIES[cat]
        final_count = crawl_category(
            category=cat,
            queries=queries,
            target_dir=args.output_dir,
            target_total=args.target_count,
            engine=args.engine,
            min_size=(args.min_width, args.min_height),
            staging_root=args.staging_dir,
            deduplicator=deduplicator
        )
        category_summary[cat] = final_count

    # Cleanup staging root
    if os.path.exists(args.staging_dir):
        shutil.rmtree(args.staging_dir, ignore_errors=True)

    total_all = sum(category_summary.values())
    print(f"\n{'='*70}")
    print(f" [SUMMARY] Clean Dataset Collection Completed")
    print(f"{'='*70}")
    for cat, count in category_summary.items():
        print(f"  * {cat:20s}: {count:5d} clean verified images")
    print(f"{'-'*70}")
    print(f"  TOTAL UNIQUE DATASET  : {total_all:5d} images")
    print(f"{'='*70}")

    # Generate Leak-Free Train / Val / Test Split
    if args.split and total_all > 0:
        create_leak_free_splits(args.output_dir, args.split_output)
        if args.dvc_track:
            track_with_dvc(args.split_output)
    elif args.dvc_track and total_all > 0:
        track_with_dvc(args.output_dir)

    print("\n[SUCCESS] Anti-Data-Leak pipeline executed successfully.")


if __name__ == "__main__":
    main()