# ML & Dataset Management Recipes (Cross-Platform: Linux, macOS, Windows)

YOLO_MODEL := "yolov8n.pt"
YOLO_BEST := "ml/AI1/model/yolov8n/best.pt"
YOLO_DATA := "ml/AI1/yolo_dataset/dataset.yaml"
RAW_DATASET := "ml/dataset/dataset_mbg_matang"
SPLIT_DATASET := "ml/AI1/dataset"
TRAIN_DIR := "ml/AI1/dataset/train"
TEST_IMG_DIR := "ml/AI1/yolo_dataset/images/test"
AI1_ONNX := "ml/AI1/model/yolov8n/ai1_vision.onnx"
AI1_INT8_ONNX := "ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx"

# Set up Python virtual environment and ML dependencies
[group('ML')]
setup-ml:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ ! -d .venv ]; then
        echo "[+] Creating Python virtual environment in .venv..."
        python3 -m venv .venv || python -m venv .venv || py -m venv .venv
    fi
    if [ -f .venv/bin/pip ]; then
        .venv/bin/pip install --upgrade pip
        .venv/bin/pip install -r ml/requirements.txt
    elif [ -f .venv/Scripts/pip.exe ]; then
        .venv/Scripts/pip.exe install --upgrade pip
        .venv/Scripts/pip.exe install -r ml/requirements.txt
    else
        pip install --upgrade pip
        pip install -r ml/requirements.txt
    fi
    echo "[✓] ML environment configured successfully."

# Scrape web images for dataset with deduplication
[group('ML')]
scrap-dataset category="all" target="1000" engine="all" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset --threads {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset --threads {{ workers }}
    else
        python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset --threads {{ workers }} || python ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset --threads {{ workers }}
    fi

# Split dataset into train/val/test subsets
[group('ML')]
split-dataset source=RAW_DATASET output=SPLIT_DATASET workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    else
        python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }} || python ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    fi

# Run domain augmentations on train set
[group('ML')]
augment-train target="500" dir=TRAIN_DIR workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    else
        python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }} || python ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    fi

# Generate YOLO annotations using zero-shot inference
[group('ML')]
prepare-yolo-ai1 batch="16" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/AI1/prepare_yolo_dataset.py --batch-size {{ batch }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/AI1/prepare_yolo_dataset.py --batch-size {{ batch }} --workers {{ workers }}
    else
        python3 ml/AI1/prepare_yolo_dataset.py --batch-size {{ batch }} --workers {{ workers }} || python ml/AI1/prepare_yolo_dataset.py --batch-size {{ batch }} --workers {{ workers }}
    fi

# Train YOLOv8n detector and export ONNX model
[group('ML')]
train-ai1 epochs="100" batch="8" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/AI1/train_AI1.py --epochs {{ epochs }} --batch-size {{ batch }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/AI1/train_AI1.py --epochs {{ epochs }} --batch-size {{ batch }} --workers {{ workers }}
    else
        python3 ml/AI1/train_AI1.py --epochs {{ epochs }} --batch-size {{ batch }} --workers {{ workers }} || python ml/AI1/train_AI1.py --epochs {{ epochs }} --batch-size {{ batch }} --workers {{ workers }}
    fi

# ------------------------------------------------------------------------------
# DIRECT ULTRALYTICS YOLO CLI COMMANDS
# ------------------------------------------------------------------------------

# Train YOLO backbone via Ultralytics CLI
[group('ML')]
yolo-train model=YOLO_MODEL data=YOLO_DATA epochs="50" batch="16" imgsz="640" device="0" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Starting Ultralytics YOLO Training: model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }}"
    W_ARG="{{ workers }}"
    if [ "$W_ARG" = "0" ]; then
        W_ARG=$(python3 ml/cpu_utils.py 2>/dev/null | grep -o '[0-9]\+' || nproc 2>/dev/null || echo 4)
    fi
    if [ -f .venv/bin/yolo ]; then
        .venv/bin/yolo detect train model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} workers=$W_ARG project=ml/AI1/runs name=yolo_train exist_ok=True
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 -m ultralytics detect train model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} workers=$W_ARG project=ml/AI1/runs name=yolo_train exist_ok=True
    elif [ -f .venv/Scripts/yolo.exe ]; then
        .venv/Scripts/yolo.exe detect train model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} workers=$W_ARG project=ml/AI1/runs name=yolo_train exist_ok=True
    else
        yolo detect train model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} workers=$W_ARG project=ml/AI1/runs name=yolo_train exist_ok=True || python3 -m ultralytics detect train model={{ model }} data={{ data }} epochs={{ epochs }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} workers=$W_ARG project=ml/AI1/runs name=yolo_train exist_ok=True
    fi

# Validate YOLO checkpoint metrics
[group('ML')]
yolo-val model=YOLO_BEST data=YOLO_DATA batch="16" imgsz="640" device="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Validating YOLO checkpoint: {{ model }} on dataset: {{ data }}"
    if [ -f .venv/bin/yolo ]; then
        .venv/bin/yolo detect val model={{ model }} data={{ data }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }}
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 -m ultralytics detect val model={{ model }} data={{ data }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }}
    elif [ -f .venv/Scripts/yolo.exe ]; then
        .venv/Scripts/yolo.exe detect val model={{ model }} data={{ data }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }}
    else
        yolo detect val model={{ model }} data={{ data }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }} || python3 -m ultralytics detect val model={{ model }} data={{ data }} batch={{ batch }} imgsz={{ imgsz }} device={{ device }}
    fi

# Export PyTorch weights to dynamic ONNX
[group('ML')]
yolo-export model=YOLO_BEST format="onnx" imgsz="640" opset="12" dynamic="True":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Exporting YOLO checkpoint: {{ model }} to {{ format }} format (Dynamic: {{ dynamic }}, Opset: {{ opset }})"
    if [ -f .venv/bin/yolo ]; then
        .venv/bin/yolo export model={{ model }} format={{ format }} imgsz={{ imgsz }} opset={{ opset }} dynamic={{ dynamic }} simplify=True
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 -m ultralytics export model={{ model }} format={{ format }} imgsz={{ imgsz }} opset={{ opset }} dynamic={{ dynamic }} simplify=True
    elif [ -f .venv/Scripts/yolo.exe ]; then
        .venv/Scripts/yolo.exe export model={{ model }} format={{ format }} imgsz={{ imgsz }} opset={{ opset }} dynamic={{ dynamic }} simplify=True
    else
        yolo export model={{ model }} format={{ format }} imgsz={{ imgsz }} opset={{ opset }} dynamic={{ dynamic }} simplify=True || python3 -m ultralytics export model={{ model }} format={{ format }} imgsz={{ imgsz }} opset={{ opset }} dynamic={{ dynamic }} simplify=True
    fi
    ONNX_SRC="${model%.pt}.onnx"
    if [ -f "$ONNX_SRC" ]; then
        mkdir -p ml/AI1/model/yolov8n
        cp -f "$ONNX_SRC" ml/AI1/model/yolov8n/yolo8n.onnx
        cp -f "$ONNX_SRC" ml/AI1/model/yolov8n/ai1_vision.onnx
        echo "[✓] Synced dynamic ONNX binary to ml/AI1/model/yolov8n/yolo8n.onnx and ml/AI1/model/yolov8n/ai1_vision.onnx"
    fi

# Run YOLO inference on test images
[group('ML')]
yolo-predict model=YOLO_BEST source=TEST_IMG_DIR conf="0.25" device="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Running YOLO Inference: model={{ model }} source={{ source }} (Conf: {{ conf }})"
    if [ -f .venv/bin/yolo ]; then
        .venv/bin/yolo detect predict model={{ model }} source={{ source }} conf={{ conf }} device={{ device }} save=True project=ml/AI1/runs name=predict exist_ok=True
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 -m ultralytics detect predict model={{ model }} source={{ source }} conf={{ conf }} device={{ device }} save=True project=ml/AI1/runs name=predict exist_ok=True
    elif [ -f .venv/Scripts/yolo.exe ]; then
        .venv/Scripts/yolo.exe detect predict model={{ model }} source={{ source }} conf={{ conf }} device={{ device }} save=True project=ml/AI1/runs name=predict exist_ok=True
    else
        yolo detect predict model={{ model }} source={{ source }} conf={{ conf }} device={{ device }} save=True project=ml/AI1/runs name=predict exist_ok=True || python3 -m ultralytics detect predict model={{ model }} source={{ source }} conf={{ conf }} device={{ device }} save=True project=ml/AI1/runs name=predict exist_ok=True
    fi

# Tune YOLO hyperparameters
[group('ML')]
yolo-tune model=YOLO_MODEL data=YOLO_DATA epochs="30" iterations="50" device="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Tuning YOLO Hyperparameters: model={{ model }} data={{ data }} (Iterations: {{ iterations }})"
    if [ -f .venv/bin/yolo ]; then
        .venv/bin/yolo detect tune model={{ model }} data={{ data }} epochs={{ epochs }} iterations={{ iterations }} device={{ device }}
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 -m ultralytics detect tune model={{ model }} data={{ data }} epochs={{ epochs }} iterations={{ iterations }} device={{ device }}
    elif [ -f .venv/Scripts/yolo.exe ]; then
        .venv/Scripts/yolo.exe detect tune model={{ model }} data={{ data }} epochs={{ epochs }} iterations={{ iterations }} device={{ device }}
    else
        yolo detect tune model={{ model }} data={{ data }} epochs={{ epochs }} iterations={{ iterations }} device={{ device }} || python3 -m ultralytics detect tune model={{ model }} data={{ data }} epochs={{ epochs }} iterations={{ iterations }} device={{ device }}
    fi

# Rebuild and deploy vision inspection container
[group('Docker')]
deploy-vision:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Rebuilding and deploying vision container with latest ONNX weights..."
    docker compose -f deploy/docker-compose.yml up -d --build vision
    echo "[✓] Vision container deployed successfully."

# Run end-to-end ML pipeline to deployment
[group('ML')]
pipeline-ai1 workers="0" batch="16" epochs="50" target="300":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "=== [1/6] Checking Raw Image Dataset ==="
    DATASET_DIR="ml/dataset/dataset_mbg_matang"
    IMG_COUNT=$(find "$DATASET_DIR" -type f \( -name "*.jpg" -o -name "*.jpeg" -o -name "*.png" -o -name "*.webp" \) 2>/dev/null | wc -l || echo 0)
    if [ "$IMG_COUNT" -lt 50 ]; then
        echo "[+] Found only $IMG_COUNT images in $DATASET_DIR. Launching scraper for {{ target }} images per category..."
        just scrap-dataset all {{ target }} all {{ workers }}
    else
        echo "[✓] Found $IMG_COUNT raw images in $DATASET_DIR."
    fi

    echo "=== [2/6] Parallel Splitting Dataset (Train/Val/Test) ==="
    just split-dataset "$DATASET_DIR" "ml/AI1/dataset" "{{ workers }}"

    echo "=== [3/6] Parallel Augmenting Training Set Safely ==="
    just augment-train "500" "ml/AI1/dataset/train" "{{ workers }}"

    echo "=== [4/6] Batched Zero-Shot YOLO Annotations & Metadata ==="
    just prepare-yolo-ai1 "{{ batch }}" "{{ workers }}"

    echo "=== [5/6] Multi-Threaded YOLOv8n Training & Dynamic ONNX Export ==="
    just train-ai1 "{{ epochs }}" "{{ batch }}" "{{ workers }}"

    echo "=== [6/6] Verifying Models & Deploying Vision Service ==="
    just generate-ncnn-cpp
    just test-model-ai1
    just deploy-vision
    echo "[✓] End-to-end ML Pipeline to Vision Deployment completed successfully!"

# Compile and run C++ NCNN model generator
[group('ML')]
generate-ncnn-cpp model="realesr-animevideov3-x2" scale="2" out="weights":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Compiling and running high-speed C++ NCNN Generator (bawal::ml)..."
    g++ -std=c++20 -O3 -I ml/include ml/src/ncnn_generator.cpp ml/ncnn_generator.cpp -o ml/ncnn_generator
    ./ml/ncnn_generator "{{ model }}" "{{ scale }}" "{{ out }}"
    echo "[✓] NCNN models successfully created and synced to {{ out }}/"

# Verify FP32 ONNX model tensor shapes
[group('ML')]
test-model-ai1 model=AI1_ONNX:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    PY_BIN="python3"
    if [ -f .venv/bin/python3 ]; then
        PY_BIN=".venv/bin/python3"
    elif [ -f .venv/Scripts/python.exe ]; then
        PY_BIN=".venv/Scripts/python.exe"
    fi
    $PY_BIN -c "import onnxruntime as ort, numpy as np; sess = ort.InferenceSession('{{ model }}', providers=['CPUExecutionProvider']); in_meta = sess.get_inputs()[0]; print(f'[✓] ONNX Model Verified: {{ model }}'); print(f'    Input : {in_meta.name} {in_meta.shape} {in_meta.type}'); dummy = np.zeros([1, 3, 640, 640], dtype=np.float32); out = sess.run(None, {in_meta.name: dummy}); print(f'    Output Shape: {out[0].shape}'); print('[✓] Model forward pass successful via ONNX Runtime CPU.')"

# Verify Quantized INT8 ONNX model tensors
[group('ML')]
test-model-ai1-int8 model=AI1_INT8_ONNX:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    PY_BIN="python3"
    if [ -f .venv/bin/python3 ]; then
        PY_BIN=".venv/bin/python3"
    elif [ -f .venv/Scripts/python.exe ]; then
        PY_BIN=".venv/Scripts/python.exe"
    fi
    $PY_BIN -c "import onnxruntime as ort, numpy as np; sess = ort.InferenceSession('{{ model }}', providers=['CPUExecutionProvider']); in_meta = sess.get_inputs()[0]; print(f'[✓] ONNX INT8 Model Verified: {{ model }}'); print(f'    Input : {in_meta.name} {in_meta.shape} {in_meta.type}'); dummy = np.zeros([1, 3, 640, 640], dtype=np.float32); out = sess.run(None, {in_meta.name: dummy}); print(f'    Output Shape: {out[0].shape}'); print('[✓] INT8 Model forward pass successful via ONNX Runtime CPU.')"

# Clean ML training run caches and intermediate weights
[group('ML')]
clean-ml:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    rm -rf ml/AI1/runs ml/AI1/yolo_dataset/__pycache__ ml/__pycache__
    echo "[✓] ML training runs and caches cleaned."

# Run local MLflow tracking server backed by PostgreSQL
[group('ML')]
mlflow-ui:
    mlflow server \
      --backend-store-uri postgresql://postgres:1234@127.0.0.1:5433/mlflow_db \
      --default-artifact-root ./ml/mlflow_artifacts \
      --host 127.0.0.1 \
      --port 5000

# Run INT8 Post-Training Quantization for YOLOv8
[group('ML')]
quantize-ai1:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python ]; then
        .venv/bin/python ml/AI1/quantize_int8.py
    elif [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/AI1/quantize_int8.py
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/AI1/quantize_int8.py
    else
        python3 ml/AI1/quantize_int8.py || python ml/AI1/quantize_int8.py
    fi

# Automated Scrapper via Duckduckgo and export it into ZIP
zip-scrapper query="nasi bento" limit="30" output="batch_inspection_images.zip":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 scripts/image_scraper.py -q "{{ query }}" -n {{ limit }} -z "{{ output }}" --clean-after
    else
        python3 scripts/image_scraper.py -q "{{ query }}" -n {{ limit }} -z "{{ output }}" --clean-after
    fi