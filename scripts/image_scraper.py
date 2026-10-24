#!/usr/bin/env python3
"""
Autonomous Multi-Engine Image Scraper & Auto-Archiver (ZIP)
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
    "nasi bento",
    "pisang segar di piring",
    "ayam goreng baki",
    "sayur bayam mangkok",
    "tahu goreng piring",
]


def sanitize_filename(name: str) -> str:
    """Membersihkan nama query agar aman dijadikan folder atau nama file."""
    return "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in name).strip("_")


def fetch_bing_image_urls(query: str, limit: int = 30) -> List[str]:
    """Mengambil URL citra resolusi penuh dari endpoint async Bing Images."""
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
        print(f"[!] Gagal menghubungi mesin Bing: {err}")

    return urls


def fetch_yahoo_image_urls(query: str, limit: int = 30) -> List[str]:
    """Fallback mesin pencari Yahoo Images jika Bing mengalami limitasi/koneksi macet."""
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
        print(f"[!] Gagal menghubungi mesin Yahoo: {err}")

    return urls


def download_single_image(url: str, save_path: Path, timeout: int = 8) -> bool:
    """Mengunduh satu gambar, memverifikasi integritas via Pillow, dan menyimpan sebagai JPEG RGB."""
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
                final_path = save_path.with_suffix(".jpg")
                rgb_img.save(final_path, "JPEG", quality=90)

            if temp_path.exists():
                temp_path.unlink()
            return True
        except Exception:
            if temp_path.exists():
                temp_path.unlink()
            return False

    except Exception:
        return False


def scrape_category(query: str, target_dir: Path, limit: int = 30, max_workers: int = 8) -> int:
    """Scraping gambar kategori baki makanan menggunakan strategi dual-engine kebal blokir ISP."""
    target_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n[+] Mengumpulkan {limit} gambar untuk query: '{query}'...")

    image_urls = fetch_bing_image_urls(query, limit=limit)

    if not image_urls:
        print("    [!] Bing tidak memberikan hasil. Beralih ke mesin cadangan (Yahoo)...")
        image_urls = fetch_yahoo_image_urls(query, limit=limit)

    if not image_urls:
        print(f"    [!] Tidak ditemukan tautan gambar valid untuk query '{query}'.")
        return 0

    print(f"    [+] Terkumpul {len(image_urls)} kandidat URL. Memulai unduhan paralel ({max_workers} thread)...")
    downloaded = 0
    futures = {}

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        for idx, url in enumerate(image_urls):
            file_stem = f"{idx:04d}_{sanitize_filename(query)[:18]}"
            dest_path = target_dir / file_stem
            future = executor.submit(download_single_image, url, dest_path)
            futures[future] = url

        for future in concurrent.futures.as_completed(futures):
            if future.result():
                downloaded += 1
                print(f"    [OK] ({downloaded}/{limit}) Gambar terverifikasi & disimpan", end="\r")
                if downloaded >= limit:
                    for f in futures:
                        f.cancel()
                    break

    print(f"\n[✓] Sukses mengunduh & memvalidasi {downloaded} gambar untuk '{query}'.")
    return downloaded


def make_zip_archive(source_dir: Path, zip_output_path: Path) -> Path:
    """Mengemas folder unduhan ke arsip .zip bersih."""
    print(f"\n[+] Mengompres direktori '{source_dir}' ke berkas ZIP...")
    zip_output_path.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(zip_output_path, "w", zipfile.ZIP_DEFLATED) as zipf:
        file_count = 0
        for root, _, files in os.walk(source_dir):
            for file in files:
                file_path = Path(root) / file
                arcname = file_path.relative_to(source_dir)
                zipf.write(file_path, arcname)
                file_count += 1

    size_mb = zip_output_path.stat().st_size / (1024 * 1024)
    print(f"[✓] Berkas ZIP selesai dibuat: '{zip_output_path.resolve()}'")
    print(f"    - Total Citra Baki : {file_count} file")
    print(f"    - Ukuran Berkas    : {size_mb:.2f} MB")
    return zip_output_path


def main():
    parser = argparse.ArgumentParser(
        description="Dual-Engine Image Scraper & Auto-ZIP Archiver (Anti-ISP Blocking)"
    )
    parser.add_argument(
        "-q", "--queries",
        nargs="+",
        default=DEFAULT_QUERIES,
        help="Daftar kata kunci pencarian (misal: 'nasi bento' 'ayam goreng baki')"
    )
    parser.add_argument(
        "-n", "--limit",
        type=int,
        default=25,
        help="Jumlah target citra valid per kategori (default: 25)"
    )
    parser.add_argument(
        "-o", "--outdir",
        type=str,
        default="downloaded_images",
        help="Direktori penyimpanan lokal gambar sebelum di-zip (default: downloaded_images)"
    )
    parser.add_argument(
        "-z", "--zipname",
        type=str,
        default="batch_inspection_images.zip",
        help="Nama berkas arsip zip keluaran (default: batch_inspection_images.zip)"
    )
    parser.add_argument(
        "-w", "--workers",
        type=int,
        default=8,
        help="Jumlah thread pekerja paralel (default: 8)"
    )
    parser.add_argument(
        "--clean-after",
        action="store_true",
        help="Hapus direktori gambar sementara setelah file ZIP selesai dibuat"
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
    print(f"Target Kategori : {len(args.queries)} query -> {args.queries}")
    print(f"Limit / Kategori: {args.limit} citra")
    print(f"Target Output   : {zip_path.resolve()}")

    for query in args.queries:
        folder_name = sanitize_filename(query)
        cat_dir = base_output / folder_name
        count = scrape_category(query, cat_dir, limit=args.limit, max_workers=args.workers)
        total_all += count

    if total_all == 0:
        print("\n[!] Tidak ada citra yang berhasil diunduh. Berkas ZIP dibatalkan.")
        sys.exit(1)

    make_zip_archive(base_output, zip_path)

    if args.clean_after:
        print(f"[+] Menghapus folder sementara '{base_output}' (--clean-after aktif)...")
        shutil.rmtree(base_output, ignore_errors=True)

    elapsed = time.time() - start_time
    print("\n" + "=" * 65)
    print(f"[✓] PIPELINE SELESAI DALAM {elapsed:.1f} DETIK!")
    print(f"[✓] Total citra baki valid: {total_all}")
    print(f"[✓] Berkas ZIP siap pakai  : {zip_path.name}")
    print("=" * 65)


if __name__ == "__main__":
    main()