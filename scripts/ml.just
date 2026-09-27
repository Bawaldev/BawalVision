# ML & Dataset Management Recipes

scrap-dataset category="all" target="1000" engine="all":
    #!/usr/bin/env bash
    if [ -f ".venv/bin/python3" ]; then
        .venv/bin/python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }}
    else
        python3 ml/scrap/scraps.py --category {{ category }} --target-count {{ target }} --engine {{ engine }}
    fi

dvc-track dir="ml/dataset/dataset_mbg_matang":
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
