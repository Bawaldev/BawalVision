#!/usr/bin/env python3
"""
BawalVision Incremental Fine-Tuning & Transfer Learning Pipeline (YOLOv8s)
==========================================================================
Specialized engine for fine-tuning an existing trained model (best.pt)
with new data, hard negatives (clean empty trays, utensils), or adapted classes.
Applies backbone layer freezing, gentle learning rates, and automated static ONNX export.
"""

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
# CONFIGURATION & HYPERPARAMETERS
# ==============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_YAML_CONFIG = SCRIPT_DIR / "yolo_dataset" / "dataset.yaml"
MODEL_DIR = SCRIPT_DIR / "model" / "yolov8s"

# Default pretrained weights priority (looks for previous best.pt first)
CANDIDATE_WEIGHTS = [
    MODEL_DIR / "best.pt",
    SCRIPT_DIR / "runs" / "ai1_yolov8s_det" / "weights" / "best.pt",
    SCRIPT_DIR.parent.parent / "weights" / "best.pt",
    SCRIPT_DIR / "yolov8s.pt",
]
DEFAULT_BASE_WEIGHTS = next((str(p) for p in CANDIDATE_WEIGHTS if p.exists()), "yolov8s.pt")

FINAL_PT_TARGET = MODEL_DIR / "best.pt"
FINAL_ONNX_TARGET = MODEL_DIR / "ai1_vision.onnx"
FINAL_YOLO8S_TARGET = MODEL_DIR / "yolov8s.onnx"

INPUT_RESOLUTION = 640
DEFAULT_BATCH_SIZE = 8
DEFAULT_EPOCHS = 30
DEFAULT_LR0 = 0.0002           # Gentle learning rate for fine-tuning to prevent catastrophic forgetting
DEFAULT_FREEZE_LAYERS = 10     # Freeze Backbone (Layers 0-9) by default; only train Neck & Head
DEFAULT_PATIENCE = 15
ONNX_OPSET = 13


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
    is_cuda = torch.cuda.is_available()

    if is_cuda:
        default_dataloader_workers = min(4, max(2, system_cores // 2))
        optimal_torch_threads = min(4, max(2, system_cores // 2))
    else:
        default_dataloader_workers = min(6, max(2, int(system_cores * 0.75)))
        optimal_torch_threads = max(2, int(system_cores * 0.75))

    parser = argparse.ArgumentParser(
        description="BawalVision Incremental Fine-Tuning & Transfer Learning Engine (YOLOv8s)"
    )
    parser.add_argument(
        "--weights", "-w",
        type=str,
        default=DEFAULT_BASE_WEIGHTS,
        help=f"Base model checkpoint to fine-tune from (default: {DEFAULT_BASE_WEIGHTS})"
    )
    parser.add_argument(
        "--epochs", "-e",
        type=int,
        default=DEFAULT_EPOCHS,
        help=f"Number of fine-tuning epochs (default: {DEFAULT_EPOCHS})"
    )
    parser.add_argument(
        "--batch-size", "-b",
        type=int,
        default=DEFAULT_BATCH_SIZE,
        help=f"Training batch size (default: {DEFAULT_BATCH_SIZE})"
    )
    parser.add_argument(
        "--lr0",
        type=float,
        default=DEFAULT_LR0,
        help=f"Initial fine-tuning learning rate (default: {DEFAULT_LR0})"
    )
    parser.add_argument(
        "--freeze",
        type=int,
        default=DEFAULT_FREEZE_LAYERS,
        help=f"Number of backbone layers to freeze (default: {DEFAULT_FREEZE_LAYERS}, 0 = unfreeze all)"
    )
    parser.add_argument(
        "--data", "-d",
        type=str,
        default=str(DEFAULT_YAML_CONFIG),
        help=f"Path to dataset.yaml specification (default: {DEFAULT_YAML_CONFIG})"
    )
    parser.add_argument(
        "--amp",
        action="store_true",
        default=True,
        help="Enable Automatic Mixed Precision (FP16) training (default: True)"
    )
    parser.add_argument(
        "--no-amp",
        action="store_true",
        help="Disable Automatic Mixed Precision"
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=0,
        help=f"DataLoader parallel worker processes (0 = auto-detect: {default_dataloader_workers})"
    )
    parser.add_argument(
        "--name",
        type=str,
        default="ai1_yolov8s_finetune",
        help="Run name for logging and plots (default: ai1_yolov8s_finetune)"
    )
    parser.add_argument(
        "--export-only",
        action="store_true",
        help="Skip fine-tuning and export given weights directly to static ONNX"
    )

    args = parser.parse_args()
    if args.workers <= 0:
        args.workers = default_dataloader_workers

    use_amp = args.amp and not args.no_amp
    base_weights_path = Path(args.weights).resolve()
    yaml_config = Path(args.data).resolve()

    torch.set_num_threads(optimal_torch_threads)
    MODEL_DIR.mkdir(parents=True, exist_ok=True)

    print("\n=======================================================")
    print("   BawalVision Fine-Tuning & Transfer Learning         ")
    print("=======================================================")
    print(f"[+] Base Weights       : {base_weights_path}")
    print(f"[+] Target Dataset     : {yaml_config}")
    print(f"[+] Epochs             : {args.epochs}")
    print(f"[+] Batch Size         : {args.batch_size}")
    print(f"[+] Learning Rate (lr0): {args.lr0} (Gentle Fine-Tune)")
    print(f"[+] Frozen Layers      : {args.freeze} (0-9 Backbone)" if args.freeze > 0 else "[+] Frozen Layers      : None (Full Fine-Tune)")
    print(f"[+] Resolution         : {INPUT_RESOLUTION}x{INPUT_RESOLUTION}")
    print(f"[+] DataLoader Workers : {args.workers}")
    print(f"[+] AMP (FP16)         : {use_amp}")
    print(f"[+] Hardware           : {'CUDA (' + torch.cuda.get_device_name(0) + ')' if is_cuda else 'Host CPU'}")
    print("=======================================================\n")

    if not args.export_only:
        if not base_weights_path.exists():
            raise FileNotFoundError(f"Base checkpoint not found at: {base_weights_path}")
        if not yaml_config.exists():
            raise FileNotFoundError(f"Dataset specification not found at: {yaml_config}")

        if torch.cuda.is_available():
            torch.cuda.empty_cache()
            if hasattr(torch.cuda, "amp") and hasattr(torch.cuda.amp, "autocast"):
                torch.backends.cuda.matmul.allow_tf32 = True
                torch.backends.cudnn.allow_tf32 = True

        device_id = "0" if is_cuda else "cpu"
        print(f"[+] Initializing YOLO with Base Checkpoint: {base_weights_path}...")
        model = YOLO(str(base_weights_path))

        print("\n=== Commencing Fine-Tuning Execution ===")
        start_time = time.time()
        train_job = None
        try:
            train_job = model.train(
                data=str(yaml_config),
                epochs=args.epochs,
                patience=DEFAULT_PATIENCE,
                batch=args.batch_size,
                imgsz=INPUT_RESOLUTION,
                device=device_id,
                workers=args.workers,
                project=str(SCRIPT_DIR / "runs"),
                name=args.name,
                exist_ok=True,
                verbose=True,
                optimizer="AdamW",
                lr0=args.lr0,
                lrf=0.01,
                cos_lr=True,
                warmup_epochs=1.0,
                freeze=args.freeze,
                amp=use_amp,
                box=5.5,
                cls=0.8,
                dfl=1.5,
                hsv_h=0.015,
                hsv_s=0.5,
                degrees=10.0,
                shear=2.0,
                scale=0.5,
                translate=0.15,
                flipud=0.0,
                fliplr=0.5,
                mosaic=0.5,         # Reduced mosaic for fine-tuning stability
                mixup=0.05,
                close_mosaic=5,
                plots=True,
                save=True,
                val=True,
            )
        except KeyboardInterrupt:
            print("\n[!] Fine-tuning interrupted by user (Ctrl+C). Saving current checkpoint...")

        elapsed = time.time() - start_time
        print(f"\n[⏱️] Fine-Tuning Runtime: {int(elapsed // 60)}m {int(elapsed % 60)}s")

        checkpoint_best = None
        if train_job is not None and hasattr(train_job, "save_dir"):
            cb = Path(train_job.save_dir) / "weights" / "best.pt"
            if cb.exists():
                checkpoint_best = cb
            else:
                cl = Path(train_job.save_dir) / "weights" / "last.pt"
                if cl.exists():
                    checkpoint_best = cl

        if checkpoint_best is None:
            candidates = list((SCRIPT_DIR / "runs" / args.name).glob("**/weights/best.pt"))
            if candidates:
                checkpoint_best = max(candidates, key=lambda p: p.stat().st_mtime)

        if checkpoint_best and checkpoint_best.exists():
            # Backup previous best if different
            if FINAL_PT_TARGET.exists() and FINAL_PT_TARGET != checkpoint_best:
                backup_pt = MODEL_DIR / "best_prev_v1.pt"
                shutil.copy2(FINAL_PT_TARGET, backup_pt)
                print(f"[✓] Previous model backed up to: {backup_pt}")

            shutil.copy2(checkpoint_best, FINAL_PT_TARGET)
            print(f"[✓] New fine-tuned PyTorch weights saved to: {FINAL_PT_TARGET}")
        else:
            print("[!] Warning: No new best.pt generated. Exporting base checkpoint.")
            checkpoint_best = base_weights_path

    # ==============================================================================
    # STATIC FP32 ONNX EXPORT
    # ==============================================================================
    export_source = FINAL_PT_TARGET if FINAL_PT_TARGET.exists() else base_weights_path
    print(f"\n=== Exporting Fine-Tuned Checkpoint ({export_source}) to Static FP32 ONNX ===")
    detection_engine = YOLO(str(export_source))

    exported_onnx = detection_engine.export(
        format="onnx",
        imgsz=INPUT_RESOLUTION,
        dynamic=False,
        opset=ONNX_OPSET,
        half=False,
        simplify=True,
    )

    if os.path.exists(exported_onnx):
        FINAL_ONNX_TARGET.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exported_onnx, FINAL_ONNX_TARGET)
        shutil.copy2(exported_onnx, FINAL_YOLO8S_TARGET)

        repo_weights = SCRIPT_DIR.parent.parent / "weights"
        if repo_weights.exists():
            shutil.copy2(exported_onnx, repo_weights / "yolov8s.onnx")
            shutil.copy2(exported_onnx, repo_weights / "yolo8s.onnx")
            shutil.copy2(exported_onnx, repo_weights / "ai1_vision.onnx")
            print(f"[✓] Synced updated ONNX to: {repo_weights / 'ai1_vision.onnx'}")

        os.remove(exported_onnx)
        print(f"[✓] Static FP32 ONNX deployed to: {FINAL_ONNX_TARGET}")

    # ==============================================================================
    # NUMERICAL PARITY SANITY CHECK
    # ==============================================================================
    print("\n=== Executing Parity Sanity Check ===")
    synthetic_frame = torch.randn(1, 3, INPUT_RESOLUTION, INPUT_RESOLUTION, dtype=torch.float32, device="cpu")

    torch_eval_model = detection_engine.model.to("cpu").eval()
    with torch.no_grad():
        torch_raw = torch_eval_model(synthetic_frame)
        if isinstance(torch_raw, (list, tuple)):
            torch_raw = torch_raw[0]
        torch_tensor = torch_raw.cpu().numpy()

    sess_options = ort.SessionOptions()
    sess_options.intra_op_num_threads = system_cores
    sess_options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    sess_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL

    ort_session = ort.InferenceSession(str(FINAL_ONNX_TARGET), sess_options, providers=["CPUExecutionProvider"])
    ort_in_name = ort_session.get_inputs()[0].name
    onnx_tensor = ort_session.run(None, {ort_in_name: synthetic_frame.numpy()})[0]

    peak_diff = float(np.max(np.abs(torch_tensor - onnx_tensor)))
    mean_diff = float(np.mean(np.abs(torch_tensor - onnx_tensor)))

    print(f" -> Peak Absolute Delta : {peak_diff:.8f}")
    print(f" -> Mean Absolute Delta : {mean_diff:.8f}")

    try:
        np.testing.assert_allclose(torch_tensor, onnx_tensor, rtol=1e-03, atol=1e-04)
        print("\n[✓] Sanity check PASSED! Model V2 is fully compliant for C++ deployment.\n")
    except AssertionError as err:
        print("\n[!] Warning: Numerical divergence detected:\n", err)


if __name__ == "__main__":
    main()

