#!/usr/bin/env python3
"""
BawalVision Leak-Free Dataset Splitter (Cross-Platform: Linux, macOS, Windows)
=============================================================================
Partitions raw image datasets into Train (70%), Validation (15%), and Test (15%)
with built-in Perceptual dHash & MD5 deduplication to guarantee zero Data Leakage.
Supports execution on Windows (cmd, PowerShell, Git Bash) and Linux/macOS.
"""

import argparse
import hashlib
import os
import random
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

# 1. Konfigurasi UTF-8 untuk Terminal Windows (PowerShell / CMD)
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass

# 2. Lokasi Direktori Kerja
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent if SCRIPT_DIR.name == "ml" else SCRIPT_DIR


def remove_readonly(func, path, exc_info):
    """Menangani error izin Read-Only di Windows saat shutil.rmtree dijalankan."""
    try:
        os.chmod(path, stat.S_IWRITE)
        func(path)
    except Exception:
        pass


def safe_rmtree(target_dir: Path):
    """Menghapus folder secara rekursif dengan aman lintas OS."""
    if not target_dir.exists():
        return
    try:
        if sys.version_info >= (3, 12):
            shutil.rmtree(
                str(target_dir),
                onexc=lambda func, path, exc: (os.chmod(path, stat.S_IWRITE), func(path))
            )
        else:
            shutil.rmtree(str(target_dir), onerror=remove_readonly)
    except Exception as e:
        print(f"[WARN] Tidak dapat menghapus folder {target_dir}: {e}")


def calculate_dhash(image: Image.Image, hash_size: int = 8) -> int:
    """
    Menghitung 64-bit difference hash (dHash) untuk perbandingan gradien visual.
    Menggunakan tobytes() agar tidak memicu DeprecationWarning pada Pillow 10-14.
    """
    resized = image.convert("L").resize((hash_size + 1, hash_size), Image.Resampling.LANCZOS)
    pixels = list(resized.tobytes())
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
    """Menghitung bit yang berbeda antara dua hash 64-bit (Hamming Distance)."""
    xor_val = h1 ^ h2
    if hasattr(xor_val, "bit_count"):
        return xor_val.bit_count()
    return bin(xor_val).count("1")


def calculate_md5(file_path: Path) -> str:
    """Menghitung hash MD5 biner file citra."""
    hasher = hashlib.md5()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def discover_categories_and_files(src_path: Path) -> dict[str, list[Path]]:
    """
    Mendeteksi kategori dan mengumpulkan berkas citra secara cerdas:
    1. Jika data bersarang di dalam partisi (test/, train/, val/) -> menggabungkan subfoldernya.
    2. Jika data sudah flat (langsung folder kelas di root sumber).
    """
    ignored_names = {"thumbs.db", "desktop.ini", ".ds_store"}
    valid_exts = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
    partition_keywords = {"train", "val", "valid", "validation", "test"}

    subdirs = [
        d for d in sorted(src_path.iterdir(), key=lambda p: p.name)
        if d.is_dir() and not d.name.startswith(".") and d.name.lower() not in ignored_names
    ]

    # Cek apakah folder sumber berisi folder partisi (train, val, test)
    partition_dirs = [d for d in subdirs if d.name.lower() in partition_keywords]
    
    is_partitioned = False
    if partition_dirs:
        for p in partition_dirs:
            children_dirs = [
                c for c in p.iterdir()
                if c.is_dir() and not c.name.startswith(".") and c.name.lower() not in ignored_names
            ]
            if children_dirs:
                is_partitioned = True
                break

    category_files_map: dict[str, list[Path]] = {}

    def extract_images_from_dir(directory: Path) -> list[Path]:
        img_list = []
        for item in directory.rglob("*"):
            if item.is_file() and not item.name.startswith(".") and item.name.lower() not in ignored_names:
                if item.suffix.lower() in valid_exts:
                    img_list.append(item)
        return img_list

    if is_partitioned:
        partition_list_str = ", ".join(d.name for d in partition_dirs)
        print(f" [INFO] Terdeteksi struktur bersarang partisi ({partition_list_str}).")
        print(f"        Mengumpulkan seluruh citra dari subfolder kelas ke pool konsolidasi...\n")
        
        # Ekstrak dari folder partisi (misal: test/banana, test/chicken, dll)
        for part in partition_dirs:
            for cat_dir in part.iterdir():
                if cat_dir.is_dir() and not cat_dir.name.startswith(".") and cat_dir.name.lower() not in ignored_names:
                    cat_name = cat_dir.name.lower().strip()
                    category_files_map.setdefault(cat_name, []).extend(extract_images_from_dir(cat_dir))

        # Ekstrak jika ada folder kelas tambahan di luar partisi
        for other_dir in subdirs:
            if other_dir.name.lower() not in partition_keywords:
                cat_name = other_dir.name.lower().strip()
                category_files_map.setdefault(cat_name, []).extend(extract_images_from_dir(other_dir))
    else:
        print(f" [INFO] Terdeteksi struktur flat (folder kelas langsung di root sumber).\n")
        for cat_dir in subdirs:
            cat_name = cat_dir.name.lower().strip()
            category_files_map[cat_name] = extract_images_from_dir(cat_dir)

    # Hilangkan duplikasi referensi berkas dan urutkan
    consolidated_map: dict[str, list[Path]] = {}
    for cat_name in sorted(category_files_map.keys()):
        seen_resolved = set()
        unique_paths = []
        for f in category_files_map[cat_name]:
            res_p = f.resolve()
            if res_p not in seen_resolved:
                seen_resolved.add(res_p)
                unique_paths.append(f)
        if unique_paths:
            consolidated_map[cat_name] = sorted(unique_paths, key=lambda p: p.name)

    return consolidated_map


def collect_clean_images(raw_files: list[Path], min_size: tuple = (100, 100), hamming_threshold: int = 4) -> list[Path]:
    """Menyaring citra rusak, dimensi terlalu kecil, dan menghapus duplikat (MD5 + dHash)."""
    min_w, min_h = min_size
    seen_md5 = set()
    seen_dhashes = []
    clean_files = []

    for file_path in raw_files:
        try:
            if file_path.stat().st_size == 0:
                continue
        except Exception:
            continue

        try:
            with Image.open(str(file_path)) as img:
                img_rgb = img.convert("RGB")
                img_rgb.load()  # Membaca ke RAM agar handle file Windows segera dilepas
                w, h = img_rgb.size
                if w < min_w or h < min_h:
                    continue

                # 1. Exact MD5 Deduplication
                md5_val = calculate_md5(file_path)
                if md5_val in seen_md5:
                    continue

                # 2. Perceptual dHash Deduplication
                img_dhash = calculate_dhash(img_rgb)
                is_near_dup = any(hamming_distance(img_dhash, h_exist) <= hamming_threshold for h_exist in seen_dhashes)

                if is_near_dup:
                    continue

                seen_md5.add(md5_val)
                seen_dhashes.append(img_dhash)
                clean_files.append(file_path)
        except Exception:
            continue

    return clean_files


def resolve_source_dir(source_arg: str) -> Path:
    """Mendeteksi direktori sumber secara adaptif lintas OS dan struktur kerja."""
    src_path = Path(source_arg).expanduser()
    if src_path.is_absolute() and src_path.exists():
        return src_path.resolve()

    candidates = [
        Path.cwd() / source_arg,
        PROJECT_ROOT / source_arg,
        SCRIPT_DIR / source_arg,
        PROJECT_ROOT / "ml" / "dataset" / "dataset_mbg_matang",
        SCRIPT_DIR / "dataset" / "dataset_mbg_matang",
        PROJECT_ROOT / "dataset" / "dataset_mbg_matang",
        SCRIPT_DIR / "dataset",
        src_path,
    ]
    for c in candidates:
        if c.exists() and c.is_dir():
            sub_items = [d for d in c.iterdir() if not d.name.startswith(".")]
            if sub_items:
                return c.resolve()
    return src_path.resolve()


def resolve_output_dir(output_arg: str) -> Path:
    """Mendeteksi direktori target penyimpanan dataset."""
    out_path = Path(output_arg).expanduser()
    if out_path.is_absolute():
        return out_path

    if output_arg == "ml/AI1/dataset" and SCRIPT_DIR.name == "ml" and Path.cwd() == SCRIPT_DIR:
        return (SCRIPT_DIR / "AI1" / "dataset").resolve()

    candidates = [
        Path.cwd() / output_arg,
        PROJECT_ROOT / output_arg,
        SCRIPT_DIR / output_arg,
        PROJECT_ROOT / "ml" / "AI1" / "dataset",
        SCRIPT_DIR / "AI1" / "dataset",
    ]
    for c in candidates:
        if c.parent.exists():
            return c.resolve()
    return (Path.cwd() / output_arg).resolve()


def split_dataset(source_dir: str, output_dir: str, train_ratio: float = 0.70,
                  val_ratio: float = 0.15, test_ratio: float = 0.15,
                  min_size: tuple = (100, 100), seed: int = 42):
    """Membagi citra secara acak berstrata ke train, val, dan test."""
    random.seed(seed)
    src_path = resolve_source_dir(source_dir)
    out_path = resolve_output_dir(output_dir)

    if not src_path.exists() or not src_path.is_dir():
        print(f"[ERROR] Direktori dataset sumber '{src_path}' tidak ditemukan.")
        sys.exit(1)

    if src_path == out_path:
        print(f"[ERROR] Direktori sumber dan output tidak boleh sama: '{src_path}'")
        sys.exit(1)

    start_time = time.time()
    print(f"\n{'='*70}")
    print(f" [DATASET SPLITTER] Anti-Leak Stratified Partition (Cross-Platform)")
    print(f" Source : {src_path}")
    print(f" Output : {out_path}")
    print(f" Ratios : {train_ratio*100:.0f}% Train | {val_ratio*100:.0f}% Val | {test_ratio*100:.0f}% Test")
    print(f"{'='*70}\n")

    # Temukan kategori & file gambar (menangani kasus gambar menumpuk di test)
    category_files_map = discover_categories_and_files(src_path)
    categories = list(category_files_map.keys())

    if not categories:
        print(f"[ERROR] Tidak ditemukan folder kategori atau berkas citra di dalam '{src_path}'.")
        sys.exit(1)

    # Inisialisasi folder target output
    for subset in ["train", "val", "test"]:
        subset_dir = out_path / subset
        subset_dir.mkdir(parents=True, exist_ok=True)
        for cat in categories:
            cat_dest = subset_dir / cat
            safe_rmtree(cat_dest)
            cat_dest.mkdir(parents=True, exist_ok=True)

    summary_stats = {"train": 0, "val": 0, "test": 0, "total_raw": 0, "total_clean": 0}

    # Proses partisi per kategori kelas
    for cat in categories:
        raw_files = category_files_map[cat]
        n_raw = len(raw_files)
        summary_stats["total_raw"] += n_raw

        # Deduplikasi & Validasi
        clean_files = collect_clean_images(raw_files, min_size=min_size)
        n_clean = len(clean_files)
        summary_stats["total_clean"] += n_clean

        if n_clean == 0:
            print(f"  * {cat:18s}: Raw={n_raw:4d} | Clean=   0 (Dilewati - Tidak ada gambar valid)")
            continue

        random.shuffle(clean_files)

        n_train = int(n_clean * train_ratio)
        n_val = int(n_clean * val_ratio)

        train_list = clean_files[:n_train]
        val_list = clean_files[n_train:n_train + n_val]
        test_list = clean_files[n_train + n_val:]

        # Salin file dengan penamaan terstandar (contoh: banana_00001.jpg)
        for subset, file_list in [("train", train_list), ("val", val_list), ("test", test_list)]:
            dest_dir = out_path / subset / cat
            for idx, file_src in enumerate(file_list, 1):
                ext = file_src.suffix.lower()
                dest_file = dest_dir / f"{cat}_{idx:05d}{ext}"
                shutil.copy2(str(file_src), str(dest_file))
            summary_stats[subset] += len(file_list)

        dropped = n_raw - n_clean
        print(f"  * {cat:18s}: Raw={n_raw:4d} | Clean={n_clean:4d} (Dropped={dropped:3d}) -> Train={len(train_list):4d} | Val={len(val_list):4d} | Test={len(test_list):4d}")

    elapsed = time.time() - start_time
    total_clean = max(1, summary_stats["total_clean"])
    print(f"\n{'='*70}")
    print(f" [SPLIT SUMMARY] (Completed in {elapsed:.2f}s)")
    print(f"  Total Raw Scanned : {summary_stats['total_raw']:5d}")
    print(f"  Total Clean Saved : {summary_stats['total_clean']:5d}")
    print(f"  -> Train Set      : {summary_stats['train']:5d} images ({summary_stats['train']/total_clean*100:.1f}%)")
    print(f"  -> Validation Set : {summary_stats['val']:5d} images ({summary_stats['val']/total_clean*100:.1f}%)")
    print(f"  -> Test Set       : {summary_stats['test']:5d} images ({summary_stats['test']/total_clean*100:.1f}%)")
    print(f"{'='*70}\n")


def track_with_dvc(output_dir: str):
    """Menghubungkan direktori partisi ke pelacakan DVC jika terpasang."""
    out_path = resolve_output_dir(output_dir)
    dvc_bin = (
        shutil.which("dvc") or
        shutil.which("dvc.exe") or
        shutil.which("dvc.cmd") or
        (PROJECT_ROOT / ".venv" / "Scripts" / "dvc.exe" if sys.platform == "win32" else None) or
        (PROJECT_ROOT / ".venv" / "bin" / "dvc" if sys.platform != "win32" else None) or
        os.path.expanduser("~/.local/bin/dvc")
    )
    if not dvc_bin or (isinstance(dvc_bin, Path) and not dvc_bin.exists()):
        return
    try:
        cmd = [str(dvc_bin), "add", str(out_path)]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        dvc_file = f"{out_path}.dvc"
        print(f"[SUCCESS] Split dataset tracked in DVC: {dvc_file}")
    except Exception:
        print(f"[NOTE] DVC tracking manual: dvc add {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="BawalVision Leak-Free Dataset Splitter (Train / Val / Test) - Cross-Platform"
    )
    parser.add_argument(
        "--source-dir", "-s",
        default="ml/dataset/dataset_mbg_matang",
        help="Direktori sumber dataset (default: ml/dataset/dataset_mbg_matang)"
    )
    parser.add_argument(
        "--output-dir", "-o",
        default="ml/AI1/dataset",
        help="Direktori tujuan penyimpanan partisi (default: ml/AI1/dataset)"
    )
    parser.add_argument(
        "--train-ratio",
        type=float,
        default=0.70,
        help="Rasio Training set (default: 0.70)"
    )
    parser.add_argument(
        "--val-ratio",
        type=float,
        default=0.15,
        help="Rasio Validation set (default: 0.15)"
    )
    parser.add_argument(
        "--test-ratio",
        type=float,
        default=0.15,
        help="Rasio Test set (default: 0.15)"
    )
    parser.add_argument(
        "--dvc-track",
        action=argparse.BooleanOptionalAction if hasattr(argparse, "BooleanOptionalAction") else "store_true",
        default=False,
        help="Lacak output menggunakan DVC (default: False)"
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