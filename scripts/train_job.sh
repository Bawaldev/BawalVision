#!/bin/bash
#SBATCH --job-name=yolov8_train
#SBATCH --output=logs/train_%j.log
#SBATCH --partition=gpu
#SBATCH --gpus=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=04:00:00

set -euo pipefail

# 1. Resolve project root directory dynamically
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Pastikan folder log sudah ada
mkdir -p "$PROJECT_ROOT/logs"

# 2. Load HPC modules jika tersedia
if command -v module &>/dev/null; then
    echo "[+] HPC Environment detected. Loading system modules..."
    module load cuda/12.1 2>/dev/null || true
    module load python/3.10 2>/dev/null || true
else
    echo "[+] Local environment detected. Skipping module loading."
fi

# 3. Activate Python virtual environment
if [ -f "$PROJECT_ROOT/.venv/bin/activate" ]; then
    source "$PROJECT_ROOT/.venv/bin/activate"
else
    echo "[!] Virtual environment not found at $PROJECT_ROOT/.venv"
    exit 1
fi

# 4. Dataset Staging ke NVMe/Fast Storage dengan Cleanup Trap
LOCAL_DATA_DIR="${SLURM_TMPDIR:-/tmp}/yolo_dataset_${SLURM_JOB_ID:-$$}"
echo "[+] Staging dataset to local fast storage ($LOCAL_DATA_DIR)..."
mkdir -p "$LOCAL_DATA_DIR"

# Pasang trap agar direktori scratch otomatis dibersihkan saat job selesai/batal
trap 'echo "[+] Cleaning up staged data..."; rm -rf "$LOCAL_DATA_DIR"' EXIT INT TERM

rsync -ah --delete ml/AI1/yolo_dataset/ "$LOCAL_DATA_DIR/"

# 5. Set hardware & thread environment variables
CPUS="${SLURM_CPUS_PER_TASK:-4}"
export OMP_NUM_THREADS="$CPUS"
export PYTORCH_CUDA_ALLOC_CONF="expandable_segments:True"

# Kalibrasi batch size aman
if [ -n "${SLURM_JOB_ID:-}" ]; then
    BATCH_SIZE=32  # Lebih aman terhadap risiko OOM pada GPU 16GB-24GB (FP32)
else
    BATCH_SIZE=8
    echo "[!] Running locally without SLURM: defaulting batch-size to $BATCH_SIZE to avoid GPU OOM."
fi

# 6. Jalankan training
echo "[+] Starting training run..."
python3 ml/AI1/train_AI1.py \
  --epochs 100 \
  --batch-size "$BATCH_SIZE" \
  --workers "$CPUS" \
  --data "$LOCAL_DATA_DIR/dataset.yaml"

echo "[+] Training complete."