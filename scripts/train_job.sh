#!/bin/bash
#SBATCH --job-name=yolov8_train
#SBATCH --output=logs/train_%j.log
#SBATCH --partition=gpu
#SBATCH --gpus=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=04:00:00

set -e

# 1. Resolve project root directory dynamically (works wherever script is called from)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Create logs directory if it doesn't exist
mkdir -p "$PROJECT_ROOT/logs"

# 2. Load HPC modules only if the environment supports them
if type module &>/dev/null; then
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

# 4. Configure dataset staging and execution settings
LOCAL_DATA_DIR="${SLURM_TMPDIR:-/tmp}/yolo_dataset"
echo "[+] Staging dataset to local fast storage ($LOCAL_DATA_DIR)..."
mkdir -p "$LOCAL_DATA_DIR"
rsync -ah ml/AI1/yolo_dataset/ "$LOCAL_DATA_DIR/"

# 5. Set hardware & thread environment variables
CPUS="${SLURM_CPUS_PER_TASK:-4}"
export OMP_NUM_THREADS="$CPUS"
export PYTORCH_CUDA_ALLOC_CONF="expandable_segments:True"

# Determine appropriate batch size (default: 64 on HPC with SLURM, 8 locally)
if [ -n "$SLURM_JOB_ID" ]; then
    BATCH_SIZE=64
else
    BATCH_SIZE=8
    echo "[!] Running locally without SLURM: defaulting batch-size to $BATCH_SIZE to avoid GPU OOM."
fi

# 6. Execute training
echo "[+] Starting training run..."
python3 ml/AI1/train_AI1.py \
  --epochs 100 \
  --batch-size "$BATCH_SIZE" \
  --workers "$CPUS" \
  --data "$LOCAL_DATA_DIR/dataset.yaml"

echo "[+] Training complete."