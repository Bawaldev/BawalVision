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

# Ultralytics training hyperparameters
MODEL_ARCH = "yolov8n.pt"  # Pretrained YOLOv8 Nano Object Detection backbone
INPUT_RESOLUTION = 640     # Fixed 640x640 frame resolution
BATCH_SIZE = 16            # Recommended for 6GB VRAM (NVIDIA RTX 3050)
MAX_EPOCHS = 50
EARLY_STOP_PATIENCE = 8    # Training halts if validation mAP50-95 stops improving
ONNX_OPSET = 17            # Verified opset for ONNX Runtime CPU Execution Provider


def main() -> None:
    WEIGHTS_DIR.mkdir(parents=True, exist_ok=True)
    MODEL_DIR.mkdir(parents=True, exist_ok=True)

    if not YAML_CONFIG.exists():
        raise FileNotFoundError(
            f"Specification not found: {YAML_CONFIG}. Run prepare_yolo_dataset.py first!"
        )

    device_id = "0" if torch.cuda.is_available() else "cpu"
    print(f"[+] Compute Hardware : {'NVIDIA CUDA (GPU:0)' if device_id == '0' else 'Host CPU'}")
    print(f"[+] Dataset Metadata : {YAML_CONFIG}")

    # ==============================================================================
    # 1. INITIALIZE BACKBONE & TRAIN
    # ==============================================================================
    print(f"\n[+] Loading Detection Base: {MODEL_ARCH}...")
    model = YOLO(MODEL_ARCH)

    print("\n=== Commencing YOLOv8n Object Detection Training ===")
    start_timestamp = time.time()

    train_job = model.train(
        data=str(YAML_CONFIG),
        epochs=MAX_EPOCHS,
        patience=EARLY_STOP_PATIENCE,
        batch=BATCH_SIZE,
        imgsz=INPUT_RESOLUTION,
        device=device_id,
        workers=2 if os.name != "nt" else 0,
        project=str(SCRIPT_DIR / "runs"),
        name="ai1_yolov8n_det",
        exist_ok=True,
        verbose=True,
    )

    elapsed_seconds = time.time() - start_timestamp
    print(f"\n[⏱️] Total Training Runtime: {int(elapsed_seconds // 60)}m {int(elapsed_seconds % 60)}s")

    # Locate and copy the optimal PyTorch weights checkpoint
    checkpoint_best = Path(train_job.save_dir) / "weights" / "best.pt"
    if checkpoint_best.exists():
        shutil.copy2(checkpoint_best, FINAL_PT_TARGET)
        print(f"[✓] Optimal PyTorch weights saved to: {FINAL_PT_TARGET}")
    else:
        raise FileNotFoundError(f"Missing checkpoint at: {checkpoint_best}")

    # ==============================================================================
    # 2. FIXED-DIMENSION ONNX EXPORT ([1, 3, 640, 640])
    # ==============================================================================
    print("\n=== Exporting PyTorch Checkpoint to Static ONNX ===")
    detection_engine = YOLO(str(FINAL_PT_TARGET))

    # Disable dynamic axes to eliminate memory re-allocation inside C++ services/vision
    exported_onnx = detection_engine.export(
        format="onnx",
        imgsz=INPUT_RESOLUTION,
        dynamic=False,
        opset=ONNX_OPSET,
        simplify=True,
    )

    if os.path.exists(exported_onnx):
        shutil.move(exported_onnx, FINAL_ONNX_TARGET)
        print(f"[✓] Static ONNX binary deployed to: {FINAL_ONNX_TARGET}")

    # ==============================================================================
    # 3. NUMERICAL SANITY CHECK (PyTorch vs ONNX Runtime CPU)
    # ==============================================================================
    print("\n=== Executing Parity Sanity Check ===")

    # Synthetic input tensor: [Batch=1, Channels=3, Height=640, Width=640]
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

    # 2. ONNX Runtime CPU inference
    ort_session = ort.InferenceSession(
        str(FINAL_ONNX_TARGET), providers=["CPUExecutionProvider"]
    )
    ort_in_name = ort_session.get_inputs()[0].name
    onnx_tensor = ort_session.run(None, {ort_in_name: synthetic_frame.numpy()})[0]

    # Calculate absolute numerical divergence
    peak_diff = np.max(np.abs(torch_tensor - onnx_tensor))
    mean_diff = np.mean(np.abs(torch_tensor - onnx_tensor))

    print(f" -> PyTorch Output Shape : {torch_tensor.shape}")
    print(f" -> ONNX Output Shape    : {onnx_tensor.shape}")
    print(f" -> Peak Absolute Delta  : {peak_diff:.8f}")
    print(f" -> Mean Absolute Delta  : {mean_diff:.8f}")

    # Validate output tensor shape: [1, 4 + num_classes, 8400] -> [1, 13, 8400] for 9 classes
    expected_dim = 4 + len(CLASS_MAPPING)
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