#!/usr/bin/env python3
"""
Autonomous Multi-Engine Image Scraper & Lightweight ZIP Archiver
BawalVision / Industrial Food Dataset Utility
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import sys
import time
import urllib.parse
import zipfile
from pathlib import Path
from typing import List

import requests
from PIL import Image

HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36"
    ),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.5",
}

DEFAULT_QUERIES = [
    "bento lunch box tray",
    "fresh banana on plate",
    "fried chicken meal tray",
    "boiled spinach bowl",
    "fried tofu dish",
]


def sanitize_filename(name: str) -> str:
    """Sanitize query string into a safe folder and file name."""
    return "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in name).strip("_")


def fetch_bing_image_urls(query: str, limit: int = 30) -> List[str]:
    """Fetch full-resolution image URLs from Bing Images async endpoint."""
    urls: List[str] = []
    encoded_query = urllib.parse.quote(query)
    search_url = f"https://www.bing.com/images/async?q={encoded_query}&first=0&count={limit * 3}&adlt=off"

    try:
        resp = requests.get(search_url, headers=HEADERS, timeout=12)
        if resp.status_code == 200:
            matches = re.findall(r'murl&quot;:&quot;(http[^&]+)&quot;', resp.text)
            for raw_url in matches:
                clean_url = raw_url.replace("\\/", "/")
                if clean_url not in urls and clean_url.startswith("http"):
                    urls.append(clean_url)
                if len(urls) >= limit * 3:
                    break
    except Exception as err:
        print(f"[!] Bing search query failed: {err}")

    return urls


def fetch_yahoo_image_urls(query: str, limit: int = 30) -> List[str]:
    """Fallback search engine using Yahoo Images if Bing is throttled or blocked."""
    urls: List[str] = []
    encoded_query = urllib.parse.quote(query)
    search_url = f"https://images.search.yahoo.com/search/images?p={encoded_query}&imgsz=medium"

    try:
        resp = requests.get(search_url, headers=HEADERS, timeout=12)
        if resp.status_code == 200:
            matches = re.findall(r'&imgurl=([^&]+)&', resp.text)
            for raw_url in matches:
                decoded = urllib.parse.unquote(raw_url)
                if decoded not in urls and decoded.startswith("http"):
                    urls.append(decoded)
                if len(urls) >= limit * 2:
                    break
    except Exception as err:
        print(f"[!] Yahoo search query failed: {err}")

    return urls


def download_single_image(
    url: str,
    save_path: Path,
    timeout: int = 8,
    max_dimension: int = 800,
    quality: int = 82,
) -> bool:
    """Download single image, verify integrity via Pillow, resize, and save as optimized RGB JPEG."""
    try:
        resp = requests.get(url, headers=HEADERS, timeout=timeout, stream=True)
        if resp.status_code != 200:
            return False

        content_type = resp.headers.get("Content-Type", "").lower()
        if "image" not in content_type and "octet-stream" not in content_type:
            return False

        temp_path = save_path.with_suffix(".tmp")
        with open(temp_path, "wb") as f:
            for chunk in resp.iter_content(chunk_size=16384):
                f.write(chunk)

        try:
            with Image.open(temp_path) as img:
                img.verify()

            with Image.open(temp_path) as img:
                rgb_img = img.convert("RGB")
                if max(rgb_img.size) > max_dimension:
                    rgb_img.thumbnail((max_dimension, max_dimension), Image.Resampling.LANCZOS)
                final_path = save_path.with_suffix(".jpg")
                rgb_img.save(final_path, "JPEG", quality=quality, optimize=True)

            if temp_path.exists():
                temp_path.unlink()
            return True
        except Exception:
            if temp_path.exists():
                temp_path.unlink()
            return False

    except Exception:
        return False


def scrape_category(
    query: str,
    target_dir: Path,
    limit: int = 30,
    max_workers: int = 8,
    max_dimension: int = 800,
    quality: int = 82,
) -> int:
    """Scrape meal tray images using an anti-blocking dual-engine strategy."""
    target_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n[+] Collecting {limit} images for query: '{query}'...")

    image_urls = fetch_bing_image_urls(query, limit=limit)

    if not image_urls:
        print("    [!] Bing returned no results. Switching to fallback search engine (Yahoo)...")
        image_urls = fetch_yahoo_image_urls(query, limit=limit)

    if not image_urls:
        print(f"    [!] No valid image URLs found for query '{query}'.")
        return 0

    print(f"    [+] Found {len(image_urls)} candidate URLs. Starting parallel downloads ({max_workers} threads)...")
    downloaded = 0
    futures = {}

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        for idx, url in enumerate(image_urls):
            file_stem = f"{idx:04d}_{sanitize_filename(query)[:18]}"
            dest_path = target_dir / file_stem
            future = executor.submit(
                download_single_image,
                url,
                dest_path,
                8,
                max_dimension,
                quality,
            )
            futures[future] = url

        for future in concurrent.futures.as_completed(futures):
            if future.result():
                downloaded += 1
                print(f"    [OK] ({downloaded}/{limit}) Image verified & saved", end="\r")
                if downloaded >= limit:
                    for f in futures:
                        f.cancel()
                    break

    print(f"\n[✓] Successfully downloaded & validated {downloaded} images for '{query}'.")
    return downloaded


def make_zip_archive(source_dir: Path, zip_output_path: Path) -> Path:
    """Package downloaded folder into a compressed .zip archive."""
    print(f"\n[+] Compressing directory '{source_dir}' into ZIP archive...")
    zip_output_path.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(zip_output_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zipf:
        file_count = 0
        for root, _, files in os.walk(source_dir):
            for file in sorted(files):
                file_path = Path(root) / file
                arcname = file_path.relative_to(source_dir)
                zipf.write(file_path, arcname)
                file_count += 1

    size_mb = zip_output_path.stat().st_size / (1024 * 1024)
    print(f"[✓] ZIP archive created successfully: '{zip_output_path.resolve()}'")
    print(f"    - Total Food Tray Images: {file_count} files")
    print(f"    - File Size             : {size_mb:.2f} MB")
    return zip_output_path


def main():
    parser = argparse.ArgumentParser(
        description="Dual-Engine Image Scraper & Lightweight ZIP Archiver (Anti-ISP Blocking)"
    )
    parser.add_argument(
        "-q", "--queries",
        nargs="+",
        default=DEFAULT_QUERIES,
        help="List of search query keywords (e.g. 'bento lunch box tray' 'fried chicken meal tray')"
    )
    parser.add_argument(
        "-n", "--limit",
        type=int,
        default=25,
        help="Target number of valid images per category (default: 25)"
    )
    parser.add_argument(
        "-o", "--outdir",
        type=str,
        default="downloaded_images",
        help="Local directory to store downloaded images before archiving (default: downloaded_images)"
    )
    parser.add_argument(
        "-z", "--zipname",
        type=str,
        default="batch_inspection_images.zip",
        help="Output ZIP archive file name (default: batch_inspection_images.zip)"
    )
    parser.add_argument(
        "-w", "--workers",
        type=int,
        default=8,
        help="Number of concurrent worker threads (default: 8)"
    )
    parser.add_argument(
        "--max-dim",
        type=int,
        default=800,
        help="Maximum width/height dimension in pixels for image compression (default: 800)"
    )
    parser.add_argument(
        "--quality",
        type=int,
        default=82,
        help="JPEG compression quality 1-100 (default: 82 for lightweight serverless ZIPs)"
    )
    parser.add_argument(
        "--clean-after",
        action="store_true",
        help="Delete temporary downloaded image directory after ZIP archive is created"
    )

    args = parser.parse_args()

    base_output = Path(args.outdir)
    base_output.mkdir(parents=True, exist_ok=True)
    zip_path = Path(args.zipname)

    total_all = 0
    start_time = time.time()

    print("=" * 65)
    print("   BAWALVISION IMAGE SCRAPER & AUTO-ZIP ARCHIVER (ANTI-ISP BLOCK)")
    print("=" * 65)
    print(f"Target Categories: {len(args.queries)} queries -> {args.queries}")
    print(f"Limit / Category : {args.limit} images")
    print(f"Max Dimension    : {args.max_dim}px (JPEG Quality: {args.quality}%)")
    print(f"Target Output    : {zip_path.resolve()}")

    for query in args.queries:
        folder_name = sanitize_filename(query)
        cat_dir = base_output / folder_name
        count = scrape_category(
            query,
            cat_dir,
            limit=args.limit,
            max_workers=args.workers,
            max_dimension=args.max_dim,
            quality=args.quality,
        )
        total_all += count

    if total_all == 0:
        print("\n[!] No images were successfully downloaded. ZIP creation aborted.")
        sys.exit(1)

    make_zip_archive(base_output, zip_path)

    if args.clean_after:
        print(f"[+] Removing temporary directory '{base_output}' (--clean-after active)...")
        shutil.rmtree(base_output, ignore_errors=True)

    elapsed = time.time() - start_time
    print("\n" + "=" * 65)
    print(f"[✓] PIPELINE COMPLETED IN {elapsed:.1f} SECONDS!")
    print(f"[✓] Total Valid Tray Images : {total_all}")
    print(f"[✓] Ready-to-Use ZIP Archive: {zip_path.name}")
    print("=" * 65)


if __name__ == "__main__":
    main()
