import os
import glob
import random
from pathlib import Path
import cv2
import numpy as np
import shutil
import onnx
import onnxruntime as ort
from onnxruntime.quantization import (
    quantize_static,
    CalibrationDataReader,
    QuantType,
    QuantFormat,
    CalibrationMethod,
)
from onnxruntime.quantization.shape_inference import quant_pre_process
class YoloCalibrationDataReader(CalibrationDataReader):
    def __init__(self, image_folder: Path, input_name: str, img_size: int = 640, max_samples: int = 100):
        extensions = ("*.jpg", "*.jpeg", "*.png")
        all_paths = []
        for ext in extensions:
            all_paths.extend(glob.glob(str(image_folder / ext)))
            all_paths.extend(glob.glob(str(image_folder / "*" / ext)))

        if not all_paths:
            raise RuntimeError(f"[-] Tidak ada gambar kalibrasi ditemukan di: {image_folder}")

        random.seed(42)
        random.shuffle(all_paths)
        self.image_paths = all_paths[:max_samples]

        self.input_name = input_name
        self.img_size = img_size
        self.data_iter = iter(self.image_paths)
        print(f"[+] Menyiapkan {len(self.image_paths)} sampel kalibrasi acak dari: {image_folder}")

    def get_next(self):
        img_path = next(self.data_iter, None)
        if img_path is None:
            return None

        img = cv2.imread(img_path)
        if img is None:
            return self.get_next()

        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

        h, w = img.shape[:2]
        scale = min(self.img_size / h, self.img_size / w)
        nw, nh = int(round(w * scale)), int(round(h * scale))
        resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)

        canvas = np.full((self.img_size, self.img_size, 3), 114, dtype=np.uint8)
        top = (self.img_size - nh) // 2
        left = (self.img_size - nw) // 2
        canvas[top : top + nh, left : left + nw, :] = resized

        blob = canvas.astype(np.float32) / 255.0
        blob = np.transpose(blob, (2, 0, 1))  # HWC -> CHW
        blob = np.expand_dims(blob, axis=0)   # [1, 3, 640, 640]

        return {self.input_name: blob}


SCRIPT_DIR = Path(__file__).resolve().parent
INPUT_ONNX = SCRIPT_DIR / "model" / "yolov8n" / "ai1_vision.onnx"
PREPROC_ONNX = SCRIPT_DIR / "model" / "yolov8n" / "ai1_vision_preproc.onnx"
OUTPUT_DIR = SCRIPT_DIR / "model" / "yolov8n_int8"
OUTPUT_ONNX = OUTPUT_DIR / "ai1_vision_int8.onnx"
CALIB_DIR = SCRIPT_DIR / "yolo_dataset" / "images" / "train"


def get_head_nodes_to_exclude(model_path: Path) -> list[str]:
    model_proto = onnx.load(str(model_path))
    exclude_set = set()
    frontier = {out.name for out in model_proto.graph.output}
    for _ in range(5):
        next_frontier = set()
        for node in model_proto.graph.node:
            if any(out in frontier for out in node.output):
                exclude_set.add(node.name)
                next_frontier.update(node.input)
        frontier = next_frontier

    sensitive_prefixes = ("model.22", "dfl", "model.15", "model.18", "model.21")
    for node in model_proto.graph.node:
        name_l = node.name.lower()
        outputs_l = " ".join(node.output).lower()
        if any(k in name_l or k in outputs_l for k in sensitive_prefixes):
            exclude_set.add(node.name)

    return list(exclude_set)

def main():
    if not INPUT_ONNX.exists():
        raise FileNotFoundError(f"Source FP32 model not found at: {INPUT_ONNX}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    print("[+] Memeriksa & meng-upgrade ONNX Opset (>= 13) + pre-processing...")
    UPGRADED_ONNX = SCRIPT_DIR / "model" / "yolov8n" / "ai1_vision_opset13_tmp.onnx"
    try:
        orig_model = onnx.load(str(INPUT_ONNX))
        current_opset = orig_model.opset_import[0].version if orig_model.opset_import else 12
        if current_opset < 13:
            print(f" -> Mengonversi ONNX Opset {current_opset} -> Opset 13 untuk dukungan Per-Channel QDQ...")
            orig_model = onnx.version_converter.convert_version(orig_model, 13)
        onnx.save(orig_model, str(UPGRADED_ONNX))

        quant_pre_process(
            input_model_path=str(UPGRADED_ONNX),
            output_model_path=str(PREPROC_ONNX),
            skip_symbolic_shape=False,
        )
        quant_source = PREPROC_ONNX
    except Exception as e:
        print(f"[!] Pre-process dilewati ({e}), menggunakan model Opset 13 langsung.")
        quant_source = UPGRADED_ONNX if UPGRADED_ONNX.exists() else INPUT_ONNX

    sess_fp32 = ort.InferenceSession(str(INPUT_ONNX), providers=["CPUExecutionProvider"])
    input_name = sess_fp32.get_inputs()[0].name

    nodes_to_exclude = get_head_nodes_to_exclude(quant_source)
    print(f"[+] Mengecualikan {len(nodes_to_exclude)} node sensitif/head agar presisi bbox & confidence tetap FP32.")

    reader = YoloCalibrationDataReader(CALIB_DIR, input_name, img_size=640, max_samples=100)

    print(f"\n=== Starting Static PTQ INT8 ({INPUT_ONNX.name}) ===")
    quantize_static(
        model_input=str(quant_source),
        model_output=str(OUTPUT_ONNX),
        calibration_data_reader=reader,
        quant_format=QuantFormat.QDQ,
        per_channel=True,
        reduce_range=False,
        calibrate_method=CalibrationMethod.MinMax,
        weight_type=QuantType.QInt8,
        activation_type=QuantType.QUInt8,
        nodes_to_exclude=nodes_to_exclude,
        extra_options={
            "WeightSymmetric": True,
            "ActivationSymmetric": False,
        },
    )

    deploy_targets = [
        SCRIPT_DIR / "model" / "ai1_vision_int8.onnx",
        SCRIPT_DIR / "model" / "yolov8n" / "ai1_vision_int8.onnx",
        SCRIPT_DIR.parent.parent / "weights" / "yolo8n_int8.onnx",
        SCRIPT_DIR.parent.parent / "weights" / "ai1_vision_int8.onnx",
    ]
    for target in deploy_targets:
        if target.parent.exists():
            shutil.copy2(OUTPUT_ONNX, target)
            print(f"[✓] Synced INT8 model to: {target}")

    if PREPROC_ONNX.exists():
        PREPROC_ONNX.unlink()
    if UPGRADED_ONNX.exists():
        UPGRADED_ONNX.unlink()

    print(f"[✓] Quantization successful: {OUTPUT_ONNX}")
    fp32_size_mb = os.path.getsize(INPUT_ONNX) / (1024 * 1024)
    int8_size_mb = os.path.getsize(OUTPUT_ONNX) / (1024 * 1024)
    reduction_pct = ((fp32_size_mb - int8_size_mb) / fp32_size_mb) * 100
    print(f" -> FP32 Model Size : {fp32_size_mb:.2f} MB")
    print(f" -> INT8 Model Size : {int8_size_mb:.2f} MB ({reduction_pct:.1f}% reduction)")
    print("\n=== Verifikasi Akurasi Deteksi (FP32 vs INT8) ===")
    sess_int8 = ort.InferenceSession(str(OUTPUT_ONNX), providers=["CPUExecutionProvider"])

    sample_reader = YoloCalibrationDataReader(CALIB_DIR, input_name, img_size=640, max_samples=5)
    sample_batch = sample_reader.get_next()

    out_fp32 = sess_fp32.run(None, sample_batch)[0]
    out_int8 = sess_int8.run(None, sample_batch)[0]

    conf_fp32 = float(np.max(out_fp32[0, 4:, :]))
    conf_int8 = float(np.max(out_int8[0, 4:, :]))

    print(f" -> Sampel Uji Max Confidence (FP32) : {conf_fp32:.4f}")
    print(f" -> Sampel Uji Max Confidence (INT8) : {conf_int8:.4f}")
    print(f" -> Output Tensor Type (INT8)        : {sess_int8.get_outputs()[0].type}")


if __name__ == "__main__":
    main()