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

# Comprehensive search queries with multi-lingual and contextual variations
# to reach 1000 - 2000+ high-quality images per category.
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


def calculate_md5(file_path: str) -> str:
    """Computes MD5 hash for image deduplication."""
    hasher = hashlib.md5()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def validate_and_deduplicate(staging_dir: str, target_category_dir: str, min_size: tuple) -> int:
    """
    Validates images with PIL, removes duplicates via MD5 hash, and moves
    valid images into the target category directory with non-colliding names.
    """
    os.makedirs(target_category_dir, exist_ok=True)
    
    # Existing hashes in target directory
    seen_hashes = set()
    for existing_file in os.listdir(target_category_dir):
        ext = os.path.splitext(existing_file)[1].lower()
        if ext in {".jpg", ".jpeg", ".png", ".webp", ".bmp"}:
            full_path = os.path.join(target_category_dir, existing_file)
            try:
                if os.path.getsize(full_path) > 0:
                    seen_hashes.add(calculate_md5(full_path))
            except OSError:
                pass

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

            # Verify image format, resolution and integrity
            try:
                with Image.open(src_path) as img:
                    img.verify()
                
                # Reopen to check dimensions (verify resets file pointer)
                with Image.open(src_path) as img:
                    width, height = img.size
                    if width < min_w or height < min_h:
                        continue
            except Exception:
                # Corrupted or unreadable image
                continue

            # Compute MD5 to prevent duplicate downloads
            file_hash = calculate_md5(src_path)
            if file_hash in seen_hashes:
                continue

            seen_hashes.add(file_hash)
            
            # Destination file name using MD5 hash (guaranteed collision-free)
            dest_name = f"{os.path.basename(target_category_dir)}_{file_hash[:12]}{ext}"
            dest_path = os.path.join(target_category_dir, dest_name)
            
            try:
                shutil.copy2(src_path, dest_path)
                added_count += 1
            except Exception:
                pass

    return added_count


def crawl_category(category: str, queries: list, target_dir: str, target_total: int,
                   engine: str, min_size: tuple, staging_root: str):
    """
    Crawls queries across engines until the category reaches target_total images
    or all queries/engines are exhausted.
    """
    category_dir = os.path.join(target_dir, category)
    os.makedirs(category_dir, exist_ok=True)
    
    current_count = len([f for f in os.listdir(category_dir) if os.path.isfile(os.path.join(category_dir, f))])
    print(f"\n{'='*70}")
    print(f" [CATEGORY] '{category.upper()}' | Current: {current_count} | Target: {target_total}")
    print(f"{'='*70}")

    if current_count >= target_total:
        print(f"[INFO] Category '{category}' already has {current_count} images (>= target {target_total}). Skipping.")
        return current_count

    staging_cat_dir = os.path.join(staging_root, category)

    # Determine engines to utilize
    engines = []
    if engine in ("bing", "all"):
        engines.append("bing")
    if engine in ("google", "all"):
        engines.append("google")
    if engine in ("baidu", "all"):
        engines.append("baidu")

    # Estimate images needed per query
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

            # Clean staging folder for this query run
            if os.path.exists(staging_cat_dir):
                shutil.rmtree(staging_cat_dir, ignore_errors=True)
            os.makedirs(staging_cat_dir, exist_ok=True)

            print(f"\n[{term_idx}/{num_queries}] Engine: {eng.upper()} | Query: '{term}' (Requesting up to {max_per_query} imgs)")

            try:
                if eng == "bing":
                    crawler = BingImageCrawler(
                        downloader_threads=8,
                        storage={"root_dir": staging_cat_dir},
                        log_level=30 # WARNING level
                    )
                elif eng == "google":
                    crawler = GoogleImageCrawler(
                        downloader_threads=8,
                        storage={"root_dir": staging_cat_dir},
                        log_level=30
                    )
                elif eng == "baidu":
                    crawler = BaiduImageCrawler(
                        downloader_threads=8,
                        storage={"root_dir": staging_cat_dir},
                        log_level=30
                    )
                else:
                    continue

                crawler.crawl(
                    keyword=term,
                    max_num=max_per_query,
                    min_size=min_size
                )
            except Exception as e:
                print(f"[WARN] Crawler error on engine {eng} for query '{term}': {e}")

            # Merge and deduplicate into main category directory
            newly_added = validate_and_deduplicate(staging_cat_dir, category_dir, min_size)
            current_count += newly_added
            print(f" -> Added: +{newly_added} unique images | Total in '{category}': {current_count}/{target_total}")

            # Politeness jitter between engine calls
            time.sleep(random.uniform(0.5, 1.5))

    # Clean up staging directory
    if os.path.exists(staging_cat_dir):
        shutil.rmtree(staging_cat_dir, ignore_errors=True)

    print(f"\n[DONE] Category '{category}' finished with {current_count} verified images.")
    return current_count


def track_with_dvc(target_dir: str) -> bool:
    """Adds the dataset directory to DVC tracking."""
    if not os.path.exists(target_dir):
        print(f"[ERROR] Target directory '{target_dir}' does not exist for DVC tracking.")
        return False
        
    dvc_bin = shutil.which("dvc") or os.path.expanduser("~/.local/bin/dvc")
    print(f"\n{'='*70}")
    print(f" [DVC] Staging and Tracking Dataset: {target_dir}")
    print(f"{'='*70}")
    
    try:
        cmd = [dvc_bin, "add", target_dir]
        print(f"[CMD] {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        if result.stdout:
            print(result.stdout)
        
        dvc_file = f"{target_dir}.dvc"
        print(f"\n[SUCCESS] Dataset tracked with DVC: {dvc_file}")
        print(f"[HINT] Next Git commit steps:")
        print(f"  git add {dvc_file} .gitignore")
        print(f"  git commit -m \"chore(data): track high-volume dataset in {target_dir}\"")
        return True
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"[WARN] Failed to automatically run DVC CLI ({e}).")
        print(f"[MANUAL] You can track it manually with:")
        print(f"  dvc add {target_dir}")
        print(f"  git add {target_dir}.dvc .gitignore")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="High-Volume Dataset Scraper with DVC Tracking (1000-2000+ images/category)"
    )
    parser.add_argument(
        "--category", "-c",
        default="all",
        help="Category to scrape (e.g. tofu, pomfret_fish, rice, chicken, or 'all')"
    )
    parser.add_argument(
        "--output-dir", "-o",
        default="ml/dataset/dataset_mbg_matang",
        help="Base directory to store downloaded dataset (default: ml/dataset/dataset_mbg_matang)"
    )
    parser.add_argument(
        "--target-count", "-t",
        type=int,
        default=1000,
        help="Target number of unique images per category (default: 1000, e.g. 1000-2000)"
    )
    parser.add_argument(
        "--engine", "-e",
        choices=["bing", "google", "all"],
        default="all",
        help="Search engine(s) to use (default: all - combines Bing + Google for max volume)"
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
        "--dvc-track",
        action="store_true",
        default=True,
        help="Automatically track dataset with DVC after scraping (default: True)"
    )
    parser.add_argument(
        "--no-dvc",
        dest="dvc_track",
        action="store_false",
        help="Skip automatic DVC tracking"
    )

    args = parser.parse_args()

    if args.category == "all":
        selected_categories = list(LARGE_DATASET_QUERIES.keys())
    else:
        selected_categories = [cat.strip() for cat in args.category.split(",") if cat.strip()]

    print(f"\n========================================================")
    print(f"  BawalVision High-Volume Dataset Scraper (1000-2000+)")
    print(f"========================================================")
    print(f"Categories: {selected_categories}")
    print(f"Target Count per Category: {args.target_count}")
    print(f"Engine Mode: {args.engine.upper()}")
    print(f"Output Base Directory: {args.output_dir}")
    print(f"Min Dimensions: {args.min_width}x{args.min_height} px")
    print(f"DVC Tracking: {args.dvc_track}")
    print(f"========================================================\n")

    os.makedirs(args.output_dir, exist_ok=True)
    os.makedirs(args.staging_dir, exist_ok=True)

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
            staging_root=args.staging_dir
        )
        category_summary[cat] = final_count

    # Cleanup staging root
    if os.path.exists(args.staging_dir):
        shutil.rmtree(args.staging_dir, ignore_errors=True)

    # Print summary
    print(f"\n{'='*70}")
    print(f" [SUMMARY] High-Volume Scraping Results")
    print(f"{'='*70}")
    total_all = 0
    for cat, count in category_summary.items():
        print(f"  * {cat:20s}: {count:5d} unique images")
        total_all += count
    print(f"{'-'*70}")
    print(f"  TOTAL DATASET SIZE    : {total_all:5d} unique images")
    print(f"{'='*70}")

    # DVC Tracking
    if args.dvc_track and total_all > 0:
        track_with_dvc(args.output_dir)

    print("\n[SUCCESS] Scraper pipeline finished successfully.")


if __name__ == "__main__":
    main()