import argparse
import os
import shutil
import time
from pathlib import Path
import numpy as np
import onnxruntime as ort
import torch
from ultralytics import YOLO

# ==============================================================================
# PIPELINE CONFIGURATION & HYPERPARAMETERS
# ==============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent

YAML_CONFIG = SCRIPT_DIR / "yolo_dataset" / "dataset.yaml"
WEIGHTS_DIR = SCRIPT_DIR / "weights"
MODEL_DIR = SCRIPT_DIR / "model"

FINAL_PT_TARGET = WEIGHTS_DIR / "best.pt"
FINAL_ONNX_TARGET = MODEL_DIR / "ai1_vision.onnx"
ROOT_WEIGHTS_TARGET = SCRIPT_DIR.parent.parent / "weights" / "yolo8n.onnx"

# Ultralytics training hyperparameters (Optimized for MBG 9 Food Categories)
MODEL_ARCH = "yolov8n.pt"  # Pretrained YOLOv8 Nano Object Detection backbone
INPUT_RESOLUTION = 640     # Fixed 640x640 frame resolution (OpenCV DNN compatible)
BATCH_SIZE = 16            # Recommended for 6GB VRAM (NVIDIA RTX 3050)
MAX_EPOCHS = 50
EARLY_STOP_PATIENCE = 12   # Extended patience for optimal mAP convergence
ONNX_OPSET = 12            # Standard ONNX opset for OpenCV DNN & ONNX Runtime C++ engine
NUM_CLASSES = 9            # banana, chicken, egg, orange, rice, spinach, tempeh, tofu, water_spinach


def main() -> None:
    try:
        from ml.cpu_utils import get_optimal_workers
    except ImportError:
        try:
            from cpu_utils import get_optimal_workers
        except ImportError:
            def get_optimal_workers(reserve_cores=0):
                return max(1, (hasattr(os, "sched_getaffinity") and len(os.sched_getaffinity(0))) or os.cpu_count() or 4)

    system_cores = get_optimal_workers()
    default_dataloader_workers = min(8, max(2, system_cores))

    parser = argparse.ArgumentParser(
        description="BawalVision Optimized YOLOv8n Training & Static FP32 ONNX Export"
    )
    parser.add_argument(
        "--epochs", "-e",
        type=int,
        default=MAX_EPOCHS,
        help=f"Number of training epochs (default: {MAX_EPOCHS})"
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=BATCH_SIZE,
        help=f"Training batch size (default: {BATCH_SIZE})"
    )
    parser.add_argument(
        "--workers", "-w",
        type=int,
        default=0,
        help=f"DataLoader parallel worker processes (0 = auto-detect: {default_dataloader_workers})"
    )
    parser.add_argument(
        "--export-only",
        action="store_true",
        help="Skip training and immediately re-export existing weights/best.pt to static ONNX"
    )

    args = parser.parse_args()
    if args.workers <= 0:
        args.workers = default_dataloader_workers

    # Configure PyTorch CPU parallel execution threads
    torch.set_num_threads(system_cores)

    WEIGHTS_DIR.mkdir(parents=True, exist_ok=True)
    MODEL_DIR.mkdir(parents=True, exist_ok=True)

    # ==============================================================================
    # 1. TRAINING PHASE (OPTIONAL VIA --export-only)
    # ==============================================================================
    if not args.export_only:
        if not YAML_CONFIG.exists():
            raise FileNotFoundError(
                f"Specification not found: {YAML_CONFIG}. Run prepare_yolo_dataset.py first!"
            )

        # Enable TF32 on Ampere+ GPUs (RTX 3050/3060/40xx)
        if torch.cuda.is_available():
            torch.backends.cuda.matmul.allow_tf32 = True
            torch.backends.cudnn.allow_tf32 = True
            torch.backends.cudnn.benchmark = True
            try:
                torch.set_float32_matmul_precision("high")
            except Exception:
                pass

        device_id = "0" if torch.cuda.is_available() else "cpu"
        compute_name = torch.cuda.get_device_name(0) if torch.cuda.is_available() else "Host CPU"
        print(f"[+] Compute Hardware    : {compute_name} (TF32/FP32 Precision Enabled)")
        print(f"[+] DataLoader Workers  : {args.workers} parallel workers | CPU Threads: {system_cores}")
        print(f"[+] Target Categories   : {NUM_CLASSES} Food Classes")

        print(f"\n[+] Loading Detection Base: {MODEL_ARCH}...")
        model = YOLO(MODEL_ARCH)

        print("\n=== Commencing YOLOv8n Object Detection Training ===")
        start_timestamp = time.time()

        train_job = model.train(
            data=str(YAML_CONFIG),
            epochs=args.epochs,
            patience=EARLY_STOP_PATIENCE,
            batch=args.batch_size,
            imgsz=INPUT_RESOLUTION,
            device=device_id,
            workers=args.workers,
            project=str(SCRIPT_DIR / "runs"),
            name="ai1_yolov8n_det",
            exist_ok=True,
            verbose=True,
            optimizer="AdamW",
            lr0=0.001,
            lrf=0.01,
            cos_lr=True,
            warmup_epochs=3.0,
            amp=False,
            box=7.5,
            cls=1.0,
            dfl=1.5,
            hsv_h=0.015,
            hsv_s=0.7,
            hsv_v=0.4,
            degrees=15.0,
            translate=0.1,
            scale=0.5,
            shear=2.0,
            flipud=0.0,
            fliplr=0.5,
            mosaic=1.0,
            mixup=0.15,
            close_mosaic=10,
            plots=True,
            save=True,
            val=True,
        )

        elapsed_seconds = time.time() - start_timestamp
        print(f"\n[⏱️] Total Training Runtime: {int(elapsed_seconds // 60)}m {int(elapsed_seconds % 60)}s")

        checkpoint_best = Path(train_job.save_dir) / "weights" / "best.pt"
        if checkpoint_best.exists():
            shutil.copy2(checkpoint_best, FINAL_PT_TARGET)
            print(f"[✓] Optimal PyTorch weights saved to: {FINAL_PT_TARGET}")
        else:
            raise FileNotFoundError(f"Missing checkpoint at: {checkpoint_best}")
    else:
        print("[+] Flag --export-only aktif: Melewati proses training.")

    # ==============================================================================
    # 2. STATIC FP32 ONNX EXPORT (OPENCV DNN COMPLIANT)
    # ==============================================================================
    if not FINAL_PT_TARGET.exists():
        # Fallback search if best.pt is inside runs directory
        runs_candidates = list((SCRIPT_DIR / "runs").glob("**/weights/best.pt"))
        if runs_candidates:
            FINAL_PT_TARGET.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(runs_candidates[0], FINAL_PT_TARGET)
        else:
            raise FileNotFoundError(f"Weights file not found at: {FINAL_PT_TARGET}. Run without --export-only first.")

    print(f"\n=== Exporting PyTorch Checkpoint ({FINAL_PT_TARGET}) to Static FP32 ONNX ===")
    detection_engine = YOLO(str(FINAL_PT_TARGET))

    # Kunci dynamic=False untuk mematikan node BOOL yang memicu error pada OpenCV DNN
    exported_onnx = detection_engine.export(
        format="onnx",
        imgsz=INPUT_RESOLUTION,
        dynamic=False,     # STATIS: Mencegah error Unsupported data type: BOOL
        opset=ONNX_OPSET,  # Opset 12
        half=False,        # FP32 murni
        simplify=True,     # Constant folding
    )

    if os.path.exists(exported_onnx):
        shutil.copy2(exported_onnx, FINAL_ONNX_TARGET)
        ROOT_WEIGHTS_TARGET.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exported_onnx, ROOT_WEIGHTS_TARGET)
        os.remove(exported_onnx)
        print(f"[✓] Static FP32 ONNX binary deployed to: {FINAL_ONNX_TARGET}")
        print(f"[✓] Deployed to Docker vision mount   : {ROOT_WEIGHTS_TARGET}")

    # ==============================================================================
    # 3. NUMERICAL SANITY CHECK (PyTorch vs ONNX Runtime Multi-Threaded CPU)
    # ==============================================================================
    print("\n=== Executing Parity Sanity Check ===")

    synthetic_frame = torch.randn(
        1, 3, INPUT_RESOLUTION, INPUT_RESOLUTION, dtype=torch.float32, device="cpu"
    )

    # 1. Native PyTorch inference
    torch_eval_model = detection_engine.model.to("cpu").eval()
    with torch.no_grad():
        torch_raw = torch_eval_model(synthetic_frame)
        if isinstance(torch_raw, (list, tuple)):
            torch_raw = torch_raw[0]
        torch_tensor = torch_raw.cpu().numpy()

    # 2. ONNX Runtime Multi-Threaded CPU inference
    sess_options = ort.SessionOptions()
    sess_options.intra_op_num_threads = system_cores
    sess_options.inter_op_num_threads = system_cores
    sess_options.execution_mode = ort.ExecutionMode.ORT_PARALLEL
    sess_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL

    ort_session = ort.InferenceSession(
        str(FINAL_ONNX_TARGET), sess_options, providers=["CPUExecutionProvider"]
    )
    ort_in_name = ort_session.get_inputs()[0].name
    onnx_tensor = ort_session.run(None, {ort_in_name: synthetic_frame.numpy()})[0]

    peak_diff = np.max(np.abs(torch_tensor - onnx_tensor))
    mean_diff = np.mean(np.abs(torch_tensor - onnx_tensor))

    print(f" -> PyTorch Output Shape : {torch_tensor.shape}")
    print(f" -> ONNX Output Shape    : {onnx_tensor.shape}")
    print(f" -> Peak Absolute Delta  : {peak_diff:.8f}")
    print(f" -> Mean Absolute Delta  : {mean_diff:.8f}")

    expected_dim = 4 + NUM_CLASSES
    assert onnx_tensor.shape == (1, expected_dim, 8400), (
        f"Unexpected output shape: {onnx_tensor.shape}. Expected: (1, {expected_dim}, 8400)"
    )

    try:
        np.testing.assert_allclose(torch_tensor, onnx_tensor, rtol=1e-03, atol=1e-04)
        print("\n[✓] Sanity check PASSED! Model is fully compliant for C++ deployment.")
    except AssertionError as err:
        print("\n[!] Warning: Numerical divergence detected between engines:")
        print(err)


if __name__ == "__main__":
    main()