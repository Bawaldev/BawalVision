# ML & Dataset Management Recipes (Cross-Platform: Linux, macOS, Windows)

scrap-dataset category="all" target="1000" engine="all":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset
    elif [ -f ".venv/Scripts/python.exe" ]; then
        .venv/Scripts/python.exe ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset
    else
        python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset || python ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset
    fi

split-dataset source="ml/dataset/dataset_mbg_matang" output="ml/AI1/dataset":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }}
    elif [ -f ".venv/Scripts/python.exe" ]; then
        .venv/Scripts/python.exe ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }}
    else
        python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }} || python ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }}
    fi

augment-train target="500" dir="ml/AI1/dataset/train":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }}
    elif [ -f ".venv/Scripts/python.exe" ]; then
        .venv/Scripts/python.exe ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }}
    else
        python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }} || python ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }}
    fi

train-ai1:
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/AI1/train_AI1.py
    elif [ -f ".venv/Scripts/python.exe" ]; then
        .venv/Scripts/python.exe ml/AI1/train_AI1.py
    else
        python3 ml/AI1/train_AI1.py || python ml/AI1/train_AI1.py
    fi

dvc-track dir="ml/AI1/dataset":
    #!/usr/bin/env bash
    if command -v dvc >/dev/null 2>&1; then
        dvc add {{ dir }}
        git add {{ dir }}.dvc .gitignore
        echo "[SUCCESS] Dataset tracked with DVC: {{ dir }}.dvc"
    elif [ -f "$HOME/.local/bin/dvc" ]; then
        "$HOME/.local/bin/dvc" add {{ dir }}
        git add {{ dir }}.dvc .gitignore
        echo "[SUCCESS] Dataset tracked with DVC: {{ dir }}.dvc"
    elif [ -f ".venv/Scripts/dvc.exe" ]; then
        .venv/Scripts/dvc.exe add {{ dir }}
        git add {{ dir }}.dvc .gitignore
        echo "[SUCCESS] Dataset tracked with DVC: {{ dir }}.dvc"
    else
        echo "[ERROR] DVC not found in PATH" >&2
        exit 1
    fi

setup-ml:
    #!/usr/bin/env bash
    if [ ! -d ".venv" ]; then
        python3 -m venv .venv || python -m venv .venv || py -m venv .venv
    fi
    if [ -f ".venv/bin/pip" ]; then
        .venv/bin/pip install --upgrade pip
        .venv/bin/pip install -r ml/requirements.txt
    elif [ -f ".venv/Scripts/pip.exe" ]; then
        .venv/Scripts/pip.exe install --upgrade pip
        .venv/Scripts/pip.exe install -r ml/requirements.txt
    else
        pip install --upgrade pip
        pip install -r ml/requirements.txt
    fi

dvc-status:
    #!/usr/bin/env bash
    if command -v dvc >/dev/null 2>&1; then
        dvc status
    elif [ -f "$HOME/.local/bin/dvc" ]; then
        "$HOME/.local/bin/dvc" status
    elif [ -f ".venv/Scripts/dvc.exe" ]; then
        .venv/Scripts/dvc.exe status
    fi
