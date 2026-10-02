# ==============================================================================
# BawalVision Machine Learning Training & ONNX Export Pipeline
# Stack: Python 3.12 (Ubuntu 24.04 LTS), PyTorch, OpenCV, ONNX, DVC
# ==============================================================================

FROM ubuntu:24.04 AS ml_pipeline

# Prevent Python from writing bytecode and enable immediate unbuffered output
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    DEBIAN_FRONTEND=noninteractive

WORKDIR /app

# Install Python 3.12, system dependencies, and create non-root user
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-venv \
    build-essential \
    libgl1 \
    libglib2.0-0 \
    ca-certificates \
    curl \
    git \
    && useradd -m -s /bin/bash -u 1002 mluser

# Create a virtual environment to safely install packages on Ubuntu 24.04 (PEP 668 compliance)
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Install Python dependencies using persistent pip cache mount
COPY ml/requirements.txt /app/requirements.txt
RUN --mount=type=cache,target=/root/.cache/pip \
    pip install --upgrade pip && \
    pip install -r /app/requirements.txt

# Copy ML pipeline and model codebase
COPY --chown=mluser:mluser ml/ /app/ml/

USER mluser

WORKDIR /app/ml

CMD ["python3", "AI1/train_AI1.py"]