# ML & Dataset Management Recipes

scrap-dataset category="all" target="1000" engine="all":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset
    else
        python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }} --split-output ml/AI1/dataset
    fi

split-dataset source="dataset_mbg_matang" output="ml/AI1/dataset":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }}
    else
        python3 ml/split_dataset.py --source-dir {{ source }} --output-dir {{ output }}
    fi

augment-train target="500" dir="ml/AI1/dataset/train":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }}
    else
        python3 ml/augment_data.py --train-dir {{ dir }} --target-count {{ target }}
    fi

train-ai1:
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/AI1/train_AI1.py
    else
        python3 ml/AI1/train_AI1.py
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
    else
        echo "[ERROR] DVC not found in PATH or ~/.local/bin/dvc" >&2
        exit 1
    fi

dvc-status:
    #!/usr/bin/env bash
    if command -v dvc >/dev/null 2>&1; then
        dvc status
    elif [ -f "$HOME/.local/bin/dvc" ]; then
        "$HOME/.local/bin/dvc" status
    fi
