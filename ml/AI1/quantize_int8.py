import os
import glob
import shutil
from pathlib import Path
import cv2
import numpy as np
import onnxruntime as ort
from onnxruntime.quantization import (
    quantize_static,
    CalibrationDataReader,
    QuantType,
    QuantFormat,
    CalibrationMethod
)

try:
    import mlflow
    MLFLOW_TRACKING_URI = os.getenv("MLFLOW_TRACKING_URI", "http://127.0.0.1:5000")
    MLFLOW_EXPERIMENT_NAME = "MBG-BawalVision-YOLOv8n"
    mlflow.set_tracking_uri(MLFLOW_TRACKING_URI)
    mlflow.set_experiment(MLFLOW_EXPERIMENT_NAME)
    MLFLOW_ENABLED = True
except Exception as e:
    print(f"[!] MLflow tracking offline/skipped ({e}). Proceeding with local INT8 quantization.")
    MLFLOW_ENABLED = False
    mlflow = None

# 1. Pipeline Reader Data Calibration 
class YoloCalibrationDataReader(CalibrationDataReader):
    def __init__(self, image_folder: Path, input_name: str, img_size: int = 640, max_samples: int = 80):
        extensions = ("*.jpg", "*.jpeg", "*.png")
        self.image_paths = []
        for ext in extensions:
            self.image_paths.extend(glob.glob(str(image_folder / ext)))
        
        self.image_paths = self.image_paths[:max_samples]
        self.input_name = input_name
        self.img_size = img_size
        self.data_iter = iter(self.image_paths)
        print(f"[+] Menyiapkan {len(self.image_paths)} sampel kalibrasi dari: {image_folder}")

    def get_next(self):
        img_path = next(self.data_iter, None)
        if img_path is None:
            return None

        # Preprocessing like  YOLOv8 C++ input pipeline
        img = cv2.imread(img_path)
        if img is None:
            return self.get_next()

        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

        h, w = img.shape[:2]
        scale = min(self.img_size / h, self.img_size / w)
        nw, nh = int(round(w* scale)), int(round(h * scale))
        resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)

        canvas = np.full((self.img_size, self.img_size, 3), 114, dtype=np.uint8)
        top = (self.img_size - nh) // 2
        left = (self.img_size - nw) // 2 
        canvas[top:top+nh, left:left+nw, :] = resized
        img = canvas

        
        blob = img.astype(np.float32) / 255.0
        blob = np.transpose(blob, (2, 0, 1))  # HWC to CHW
        blob = np.expand_dims(blob, axis=0)   # [1, 3, 640, 640]

        return {self.input_name: blob}

# 2. Quantitze main funct
SCRIPT_DIR = Path(__file__).resolve().parent
INPUT_ONNX = SCRIPT_DIR / "model" / "ai1_vision.onnx"
OUTPUT_ONNX = SCRIPT_DIR / "model" / "ai1_vision_int8.onnx"
CALIB_DIR = SCRIPT_DIR / "yolo_dataset" / "images" / "train"
DOCKER_WEIGHTS_INT8 = SCRIPT_DIR.parent.parent / "weights" / "yolo8n_int8.onnx"
DOCKER_WEIGHTS_AI1_INT8 = SCRIPT_DIR.parent.parent / "weights" / "ai1_vision_int8.onnx"
DOCKER_WEIGHTS_FALLBACK = SCRIPT_DIR.parent.parent / "weights" / "yolo8n.onnx"

def main():
    if not INPUT_ONNX.exists():
        raise FileNotFoundError(f"Source FP32 model not found at: {INPUT_ONNX}")

    if MLFLOW_ENABLED and mlflow:
        mlflow.set_tracking_uri(MLFLOW_TRACKING_URI)
        mlflow.set_experiment(MLFLOW_EXPERIMENT_NAME)

    # Retrieve input layer name from ONNX graph
    session = ort.InferenceSession(str(INPUT_ONNX), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name

    import onnx
    model_proto = onnx.load(str(INPUT_ONNX))
    nodes_to_exclude = [n.name for n in model_proto.graph.node if 'model.22' in n.name or 'Sigmoid' in n.name or 'dfl' in n.name.lower()]
    print(f"[+] Excluding {len(nodes_to_exclude)} sensitive detection head nodes from INT8 quantization to preserve bbox/confidence precision.")

    reader = YoloCalibrationDataReader(CALIB_DIR, input_name, img_size=640, max_samples=80)
    print(f"\n=== Starting Static PTQ INT8 ({INPUT_ONNX.name}) ===")
    quantize_static(
        model_input=str(INPUT_ONNX),
        model_output=str(OUTPUT_ONNX),
        calibration_data_reader=reader,
        quant_format=QuantFormat.QDQ,     # Format QDQ optimal for AVX2/VNNI accel
        per_channel=True,                # Universal per-tensor quantization (broad compatibility across ONNX opsets)
        calibrate_method=CalibrationMethod.Entropy,  # Entropy calibration for better accuracy
        weight_type=QuantType.QInt8,       
        activation_type=QuantType.QUInt8, # UINT8 Activation
        nodes_to_exclude=nodes_to_exclude
    )
    print(f"[✓] Quantization successful: {OUTPUT_ONNX}")

    # Measure file size reduction
    fp32_size_mb = os.path.getsize(INPUT_ONNX) / (1024 * 1024)
    int8_size_mb = os.path.getsize(OUTPUT_ONNX) / (1024 * 1024)
    reduction_pct = ((fp32_size_mb - int8_size_mb) / fp32_size_mb) * 100

    print(f" -> FP32 Model Size : {fp32_size_mb:.2f} MB")
    print(f" -> INT8 Model Size : {int8_size_mb:.2f} MB ({reduction_pct:.1f}% reduction)")

    if MLFLOW_ENABLED and mlflow:
        with mlflow.start_run(run_name="yolov8n_ptq_int8"):
            mlflow.log_params({
                "quantization_type": "Static_PTQ_QDQ",
                "per_channel": True,
                "calibration_samples": 80
            })
            mlflow.log_metrics({
                "model_fp32_size_mb": fp32_size_mb,
                "model_int8_size_mb": int8_size_mb,
                "size_reduction_pct": reduction_pct
            })
            mlflow.log_artifact(str(OUTPUT_ONNX), artifact_path="model_int8")

    # Copy to Docker Vision weights folder
    for target in (DOCKER_WEIGHTS_INT8, DOCKER_WEIGHTS_AI1_INT8):
        if target.parent.exists():
            shutil.copy2(OUTPUT_ONNX, target)
            print(f"[✓] INT8 model synced to: {target}")

if __name__ == "__main__":
    main()