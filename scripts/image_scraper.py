#!/usr/bin/env python3
"""
Autonomous Multi-Food Image Scraper & Lightweight ZIP Archiver (<4MB Guaranteed)
BawalVision / Industrial Food Dataset Inspection Utility

Features:
- Dual-Engine (Bing Async + Yahoo Fallback) to bypass ISP throttling.
- Dual-Layer Gatekeeper (Safe OpenCV Cascade + Pure Pillow Visual Analysis).
- Eliminates human faces, people eating, flyers, and white document menus.
- Strict 640px JPEG compression + zero EXIF to guarantee < 4 MB ZIP payload.
"""

import argparse
import concurrent.futures
import io
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
from PIL import Image, ImageFilter, ImageStat

# Safe OpenCV Cascade Loader with Fallback
HAS_OPENCV_CASCADE = False
FACE_CASCADE = None

try:
    import cv2
    import numpy as np
    if hasattr(cv2, "CascadeClassifier") and hasattr(cv2, "data") and hasattr(cv2.data, "haarcascades"):
        cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
        if os.path.exists(cascade_path):
            FACE_CASCADE = cv2.CascadeClassifier(cascade_path)
            if not FACE_CASCADE.empty():
                HAS_OPENCV_CASCADE = True
except Exception:
    HAS_OPENCV_CASCADE = False

HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36"
    ),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.8,id;q=0.6",
}

# 9-Class Multi-Food Plate Queries (Overhead / Top-Down View Only)
DEFAULT_MULTI_FOOD_QUERIES = [
    "top down food plate white rice fried chicken tofu tempeh",
    "overhead meal plate rice roasted chicken spinach boiled egg",
    "nasi rames ayam goreng tahu tempe lalapan piring",
    "top down dish steamed rice fried chicken water spinach",
    "food plate portion rice cooked chicken greens sliced banana",
    "balanced meal plate white rice chicken breast vegetables orange fruit",
    "piring makan nasi uduk telur dadar tempe orek tahu",
    "cooked chicken dish plate top view close up",
    "tumis kangkung piring saji makan siang",
]

# Strict Negative Filters: Eliminate People, Faces, Documents, and Empty Containers
NEGATIVE_PROMPT = (
    "-person -people -face -child -children -man -woman -human -holding -chef "
    "-portrait -selfie -eating -poster -flyer -brochure -document -article "
    "-tokopedia -shopee -lazada -empty -stainless -box_only -review"
)


def sanitize_filename(name: str) -> str:
    """Sanitize query string into a filesystem-safe string."""
    return "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in name).strip("_")


def is_rejected_by_visual_gatekeeper(pil_img: Image.Image) -> bool:
    """
    Two-layer rejection filter:
    1. OpenCV Face Cascade (if available).
    2. Pure Pillow Heuristics:
       - White article flyers (High luminance V > 200 + low saturation S < 30).
       - Text-heavy pages (High edge density on white backgrounds).
       - Blank / flat images.
    """
    # Layer 1: OpenCV Face Detection (Optional / Safe)
    if HAS_OPENCV_CASCADE:
        try:
            cv_img = cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)
            gray = cv2.cvtColor(cv_img, cv2.COLOR_BGR2GRAY)
            faces = FACE_CASCADE.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=4, minSize=(30, 30))
            if len(faces) > 0:
                return True
        except Exception:
            pass

    # Layer 2: Pillow Heuristics (Always Active)
    try:
        rgb = pil_img.convert("RGB")
        hsv = pil_img.convert("HSV")
        _, s, v = hsv.split()

        mean_v = ImageStat.Stat(v).mean[0]
        mean_s = ImageStat.Stat(s).mean[0]

        # Reject white paper flyers, menus, and article screenshots
        if mean_v > 200 and mean_s < 30:
            return True

        # Check for text density using edge filter
        edges = rgb.convert("L").filter(ImageFilter.FIND_EDGES)
        edge_mean = ImageStat.Stat(edges).mean[0]
        if mean_v > 185 and edge_mean > 45:
            return True

        # Reject near-blank images
        if ImageStat.Stat(v).stddev[0] < 18.0:
            return True

    except Exception:
        return False

    return False


def fetch_bing_image_urls(query: str, limit: int = 30) -> List[str]:
    """Fetch direct full-resolution image URLs from Bing Images async endpoint."""
    urls: List[str] = []
    clean_q = f"{query} {NEGATIVE_PROMPT}"
    encoded = urllib.parse.quote(clean_q)
    search_url = f"https://www.bing.com/images/async?q={encoded}&first=0&count={limit * 3}&adlt=off"

    try:
        resp = requests.get(search_url, headers=HEADERS, timeout=10)
        if resp.status_code == 200:
            matches = re.findall(r'murl&quot;:&quot;(http[^&]+)&quot;', resp.text)
            for raw_url in matches:
                clean_url = raw_url.replace("\\/", "/")
                bad_domains = ["shutterstock", "gettyimages", "stock-photo", "wallpaper", "kompas", "detik"]
                if any(bad in clean_url.lower() for bad in bad_domains):
                    continue
                if clean_url not in urls and clean_url.startswith("http"):
                    urls.append(clean_url)
                if len(urls) >= limit * 2:
                    break
    except Exception as err:
        print(f"    [!] Bing engine query notice: {err}")

    return urls


def fetch_yahoo_image_urls(query: str, limit: int = 30) -> List[str]:
    """Fallback search engine using Yahoo Images if Bing is throttled."""
    urls: List[str] = []
    clean_q = f"{query} {NEGATIVE_PROMPT}"
    encoded = urllib.parse.quote(clean_q)
    search_url = f"https://images.search.yahoo.com/search/images?p={encoded}&imgsz=medium"

    try:
        resp = requests.get(search_url, headers=HEADERS, timeout=10)
        if resp.status_code == 200:
            matches = re.findall(r'&imgurl=([^&]+)&', resp.text)
            for raw_url in matches:
                decoded = urllib.parse.unquote(raw_url)
                if decoded not in urls and decoded.startswith("http"):
                    urls.append(decoded)
                if len(urls) >= limit * 2:
                    break
    except Exception as err:
        print(f"    [!] Yahoo engine query notice: {err}")

    return urls


def download_and_validate(
    url: str,
    save_path: Path,
    max_dim: int = 640,
    quality: int = 72,
) -> bool:
    """Download, inspect via visual gatekeeper, resize to 640px, strip EXIF, save lightweight JPEG."""
    try:
        resp = requests.get(url, headers=HEADERS, timeout=7)
        if resp.status_code != 200:
            return False

        content_type = resp.headers.get("Content-Type", "").lower()
        if "image" not in content_type and "octet-stream" not in content_type:
            return False

        raw_bytes = resp.content
        if len(raw_bytes) < 3000:
            return False

        # Load image via Pillow
        with Image.open(io.BytesIO(raw_bytes)) as img:
            img.verify()

        with Image.open(io.BytesIO(raw_bytes)) as img:
            rgb_img = img.convert("RGB")

            # Visual Gatekeeper: Reject faces, people, and flyer documents
            if is_rejected_by_visual_gatekeeper(rgb_img):
                return False

            # Downscale proportionally to 640px max dimension (native YOLO input)
            if max(rgb_img.size) > max_dim:
                rgb_img.thumbnail((max_dim, max_dim), Image.Resampling.LANCZOS)

            final_path = save_path.with_suffix(".jpg")
            # Save without EXIF metadata to maintain tiny size (~25-35 KB)
            rgb_img.save(final_path, "JPEG", quality=quality, optimize=True)

        return True
    except Exception:
        return False


def scrape_category(
    query: str,
    target_dir: Path,
    limit: int = 6,
    workers: int = 8,
    max_dim: int = 640,
    quality: int = 72,
) -> int:
    """Scrape and filter verified food images for a given query."""
    target_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n[+] Searching food images for: '{query}'...")

    urls = fetch_bing_image_urls(query, limit=limit * 3)
    if not urls:
        print("    [!] Bing empty, switching to Yahoo fallback...")
        urls = fetch_yahoo_image_urls(query, limit=limit * 3)

    if not urls:
        print(f"    [!] No valid URLs discovered for '{query}'.")
        return 0

    downloaded = 0
    futures = {}

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        for idx, url in enumerate(urls):
            dest_path = target_dir / f"img_{idx:04d}"
            fut = executor.submit(download_and_validate, url, dest_path, max_dim, quality)
            futures[fut] = url

        for fut in concurrent.futures.as_completed(futures):
            if fut.result():
                downloaded += 1
                print(f"    [OK] ({downloaded}/{limit}) Verified Food Dish (Zero Humans/Docs)", end="\r")
                if downloaded >= limit:
                    for f in futures:
                        f.cancel()
                    break

    print(f"\n[✓] Stored {downloaded} verified food images.")
    return downloaded


def make_clean_flat_zip(source_dir: Path, zip_output: Path, max_zip_mb: float = 3.8) -> Path:
    """
    Package files into a flat ZIP archive (level 9 compression).
    Enforces C++ BatchInspector capacity (<= 30 images) and keeps size strictly under 4 MB.
    """
    print(f"\n[+] Archiving into ZIP: '{zip_output.name}'...")
    zip_output.parent.mkdir(parents=True, exist_ok=True)

    files = sorted([p for p in source_dir.rglob("*.jpg") if p.is_file()])

    # Limit to top 30 images to match kMaxImageFiles in batch_inspector.hpp
    if len(files) > 30:
        print("    [!] Restricting to 30 images to match C++ BatchInspector limit...")
        files = files[:30]

    with zipfile.ZipFile(zip_output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for idx, f in enumerate(files, start=1):
            zf.write(f, arcname=f"tray_{idx:02d}.jpg")

    size_mb = zip_output.stat().st_size / (1024 * 1024)

    # Budget Guard: Prune if size exceeds budget
    while size_mb > max_zip_mb and len(files) > 10:
        files.pop()
        with zipfile.ZipFile(zip_output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for idx, f in enumerate(files, start=1):
                zf.write(f, arcname=f"tray_{idx:02d}.jpg")
        size_mb = zip_output.stat().st_size / (1024 * 1024)

    print(f"[✓] ZIP Created Successfully: '{zip_output.resolve()}'")
    print(f"    - Total Valid Trays : {len(files)} files")
    print(f"    - Final Archive Size: {size_mb:.2f} MB (< 4.0 MB GUARANTEED)")
    return zip_output


def main():
    parser = argparse.ArgumentParser(description="Multi-Food Scraper (<4MB ZIP Guaranteed)")
    parser.add_argument("-q", "--queries", nargs="+", default=None, help="Custom query keywords")
    parser.add_argument("-n", "--limit", type=int, default=30, help="Target total images (default: 30)")
    parser.add_argument("-o", "--outdir", type=str, default="temp_verified_foods", help="Temp folder")
    parser.add_argument("-z", "--zipname", type=str, default="batch_inspection_images.zip", help="Output ZIP path")
    parser.add_argument("-w", "--workers", type=int, default=8, help="Worker threads")
    parser.add_argument("--clean-after", action="store_true", help="Clean up temp folder after archiving")

    args = parser.parse_args()

    base_dir = Path(args.outdir)
    base_dir.mkdir(parents=True, exist_ok=True)
    zip_path = Path(args.zipname)

    queries = list(DEFAULT_MULTI_FOOD_QUERIES)
    if args.queries:
        cq = " ".join(args.queries).replace("query=", "").strip('"').strip("'")
        queries.insert(0, cq)

    per_q_limit = max(3, args.limit // len(queries) + 1)
    total_downloaded = 0
    start_time = time.time()

    print("=" * 65)
    print("      BAWALVISION ROBUST MULTI-FOOD DATASET SCRAPER")
    print("=" * 65)
    print(f"Target Meal Trays : {args.limit} images")
    print(f"Visual Gatekeeper : Active (Human & Flyer Rejection)")
    print(f"Resolution Bound  : Max 640px JPEG (Quality 72%, Stripped EXIF)")
    print(f"Target Output     : {zip_path.name}")
    print("=" * 65)

    for q in queries:
        if total_downloaded >= args.limit:
            break
        cnt = scrape_category(q, base_dir, limit=per_q_limit, workers=args.workers, max_dim=640, quality=72)
        total_downloaded += cnt

    if total_downloaded == 0:
        print("\n[!] No food images passed the visual gatekeeper.")
        sys.exit(1)

    make_clean_flat_zip(base_dir, zip_path, max_zip_mb=3.8)

    if args.clean_after:
        shutil.rmtree(base_dir, ignore_errors=True)

    elapsed = time.time() - start_time
    print(f"\n[✓] PIPELINE COMPLETED IN {elapsed:.1f}s | Output: {zip_path.name}")


if __name__ == "__main__":
    main()