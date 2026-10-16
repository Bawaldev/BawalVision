# ML & Dataset Management Recipes (Cross-Platform: Linux, macOS, Windows)

# Setup virtual environment and install ML requirements
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

# Multi-engine high-volume web image scraper with dHash deduplication (workers=0: auto-detect max cores)
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

# Stratified Train/Val/Test dataset splitter with multi-core parallel hashing & deduplication (workers=0: auto-detect max cores)
split-dataset source="ml/dataset/dataset_mbg_matang" output="ml/AI1/dataset" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    else
        python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }} || python ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} --workers {{ workers }}
    fi

# Multi-threaded parallel domain-specific image augmentation applied exclusively to train set (workers=0: auto-detect max cores)
augment-train target="500" dir="ml/AI1/dataset/train" workers="0":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    if [ -f .venv/bin/python3 ]; then
        .venv/bin/python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    elif [ -f .venv/Scripts/python.exe ]; then
        .venv/Scripts/python.exe ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    else
        python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }} || python ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} --workers {{ workers }}
    fi

# Batched zero-shot YOLO annotation generator with parallel file I/O (workers=0: auto-detect max cores)
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

# Train YOLOv8n detector with multi-core DataLoader workers & multi-threaded ONNX parity verification (workers=0: auto-detect max cores)
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

# Train any YOLO model backbone via Ultralytics CLI / Module
yolo-train model="yolov8n.pt" data="ml/AI1/yolo_dataset/dataset.yaml" epochs="50" batch="16" imgsz="640" device="0" workers="0":
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

# Validate and compute mAP50 / mAP50-95 metrics on validation set
yolo-val model="ml/AI1/model/yolov8n/best.pt" data="ml/AI1/yolo_dataset/dataset.yaml" batch="16" imgsz="640" device="0":
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

# Export PyTorch weights to dynamic ONNX binary (Ultralytics Dynamic Detection)
yolo-export model="ml/AI1/model/yolov8n/best.pt" format="onnx" imgsz="640" opset="12" dynamic="True":
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

# Run inference / prediction on test images
yolo-predict model="ml/AI1/model/yolov8n/best.pt" source="ml/AI1/yolo_dataset/images/test" conf="0.25" device="0":
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

# Hyperparameter search and auto-tuning
yolo-tune model="yolov8n.pt" data="ml/AI1/yolo_dataset/dataset.yaml" epochs="30" iterations="50" device="0":
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

# Rebuild and reload the vision inspection container
deploy-vision:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Rebuilding and deploying vision container with latest ONNX weights..."
    docker compose -f deploy/docker-compose.yml up -d --build vision
    echo "[✓] Vision container deployed successfully."

# End-to-end autonomous parallel pipeline: data gathering -> split -> augment -> annotate -> train & export ONNX -> deploy vision (workers=0: auto-detect max cores)
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

# Compile and run high-speed C++ NCNN model generator (.param and .bin)
generate-ncnn-cpp model="realesr-animevideov3-x2" scale="2" out="weights":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    echo "[+] Compiling and running high-speed C++ NCNN Generator (bawal::ml)..."
    g++ -std=c++20 -O3 -I ml/include ml/src/ncnn_generator.cpp ml/ncnn_generator.cpp -o ml/ncnn_generator
    ./ml/ncnn_generator "{{ model }}" "{{ scale }}" "{{ out }}"
    echo "[✓] NCNN models successfully created and synced to {{ out }}/"


# Sanity check and inspect ONNX model tensors and execution provider
test-model-ai1 model="ml/AI1/model/yolov8n/ai1_vision.onnx":
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    PY_BIN="python3"
    if [ -f .venv/bin/python3 ]; then
        PY_BIN=".venv/bin/python3"
    elif [ -f .venv/Scripts/python.exe ]; then
        PY_BIN=".venv/Scripts/python.exe"
    fi
    $PY_BIN -c "import onnxruntime as ort, numpy as np; sess = ort.InferenceSession('{{ model }}', providers=['CPUExecutionProvider']); in_meta = sess.get_inputs()[0]; print(f'[✓] ONNX Model Verified: {{ model }}'); print(f'    Input : {in_meta.name} {in_meta.shape} {in_meta.type}'); dummy = np.zeros([1, 3, 640, 640], dtype=np.float32); out = sess.run(None, {in_meta.name: dummy}); print(f'    Output Shape: {out[0].shape}'); print('[✓] Model forward pass successful via ONNX Runtime CPU.')"


# Clean training run caches, temporary weights, and intermediate predictions
clean-ml:
    #!/usr/bin/env bash
    cd {{ justfile_directory() }}
    rm -rf ml/AI1/runs ml/AI1/yolo_dataset/__pycache__ ml/__pycache__
    echo "[✓] ML training runs and caches cleaned."


# Running MLflow tracking local server via PostgreSQL
mlflow-ui:
    mlflow server \
      --backend-store-uri postgresql://postgres:1234@127.0.0.1:5433/mlflow_db \
      --default-artifact-root ./ml/mlflow_artifacts \
      --host 127.0.0.1 \
      --port 5000


# Run INT8 Post-Training Quantization for YOLOv8
quantize-ai1:
    python ml/AI1/quantize_int8.py