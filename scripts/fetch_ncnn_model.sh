#!/usr/bin/env bash

set -Eeuo pipefail

# Usage:
#   fetch_ncnn_model.sh [model_name] [output_dir] [generator_path] [scale]
#
# Optional environment variables:
#   ALLOW_SYNTHETIC_FALLBACK=0|1
#   MIN_PARAM_BYTES=1000
#   MIN_BIN_BYTES=5000000

MODEL_NAME="${1:-realesr-general-x4v3}"
OUTPUT_DIR="${2:-/app/weights}"
GENERATOR_BIN="${3:-/usr/local/bin/ncnn_generator}"
SCALE="${4:-4}"

ALLOW_SYNTHETIC_FALLBACK="${ALLOW_SYNTHETIC_FALLBACK:-0}"
MIN_PARAM_BYTES="${MIN_PARAM_BYTES:-1000}"
MIN_BIN_BYTES="${MIN_BIN_BYTES:-5000000}"

log() {
    printf '[INFO] %s\n' "$*"
}

warn() {
    printf '[WARN] %s\n' "$*" >&2
}

die() {
    printf '[ERROR] %s\n' "$*" >&2
    exit 1
}

# --------------------------------------------------------------------
# 1. Validate arguments and dependencies
# --------------------------------------------------------------------

[[ "$MODEL_NAME" =~ ^[a-zA-Z0-9._-]+$ ]] ||
    die "Invalid model name: $MODEL_NAME"

[[ "$SCALE" =~ ^[1-9][0-9]*$ ]] ||
    die "Scale must be a positive integer."

[[ "$ALLOW_SYNTHETIC_FALLBACK" == "0" ||
   "$ALLOW_SYNTHETIC_FALLBACK" == "1" ]] ||
    die "ALLOW_SYNTHETIC_FALLBACK must be 0 or 1."

[[ "$MIN_PARAM_BYTES" =~ ^[1-9][0-9]*$ ]] ||
    die "MIN_PARAM_BYTES must be a positive integer."

[[ "$MIN_BIN_BYTES" =~ ^[1-9][0-9]*$ ]] ||
    die "MIN_BIN_BYTES must be a positive integer."

command -v curl >/dev/null 2>&1 ||
    die "curl is required but was not found."

mkdir -p -- "$OUTPUT_DIR"
OUTPUT_DIR="$(cd -- "$OUTPUT_DIR" && pwd)"

PARAM_PATH="${OUTPUT_DIR}/${MODEL_NAME}.param"
BIN_PATH="${OUTPUT_DIR}/${MODEL_NAME}.bin"

# Keep temporary files on the same filesystem as the output files.
TMP_DIR="$(mktemp -d "${OUTPUT_DIR}/.${MODEL_NAME}.download.XXXXXX")"

cleanup() {
    rm -rf -- "$TMP_DIR"
}

trap cleanup EXIT

# --------------------------------------------------------------------
# 2. Define model validation helpers
# --------------------------------------------------------------------

validate_param() {
    local file="$1"
    local min_bytes="${2:-$MIN_PARAM_BYTES}"
    local size
    local magic

    [[ -f "$file" && -s "$file" ]] || return 1

    size="$(stat -c '%s' "$file")"
    (( size >= min_bytes )) || return 1

    # NCNN text parameter files start with this magic number.
    magic="$(head -n 1 "$file" | tr -d '\r')"
    [[ "$magic" == "7767517" ]] || return 1

    # The second line should contain the layer and blob counts.
    awk '
        NR == 2 {
            if ($1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/) {
                valid = 1
            }
            exit
        }
        END {
            exit !valid
        }
    ' "$file"
}

validate_download_pair() {
    local param_file="$1"
    local bin_file="$2"
    local bin_size

    validate_param "$param_file" || return 1

    [[ -f "$bin_file" && -s "$bin_file" ]] || return 1

    bin_size="$(stat -c '%s' "$bin_file")"
    (( bin_size >= MIN_BIN_BYTES ))
}

validate_synthetic_pair() {
    local param_file="$1"
    local bin_file="$2"

    validate_param "$param_file" 100 || return 1

    # Synthetic files are expected to be much smaller than
    # the pretrained RealESRGAN model.
    [[ -f "$bin_file" && -s "$bin_file" ]]
}

# --------------------------------------------------------------------
# 3. Reuse an existing valid pretrained model
# --------------------------------------------------------------------

if validate_download_pair "$PARAM_PATH" "$BIN_PATH"; then
    log "A valid model pair already exists."
    log "PARAM: $PARAM_PATH"
    log "BIN:   $BIN_PATH"
    exit 0
fi

log "No valid existing model pair found."
log "Starting model download..."

# --------------------------------------------------------------------
# 4. Download and validate one complete source pair at a time
# --------------------------------------------------------------------

download_from_source() {
    local base_url="$1"
    local param_tmp="${TMP_DIR}/${MODEL_NAME}.param"
    local bin_tmp="${TMP_DIR}/${MODEL_NAME}.bin"

    rm -f -- "$param_tmp" "$bin_tmp"

    log "Trying source: $base_url"

    if ! curl \
        --fail \
        --location \
        --silent \
        --show-error \
        --retry 2 \
        --retry-delay 1 \
        --retry-connrefused \
        --connect-timeout 20 \
        --max-time 120 \
        --output "$param_tmp" \
        "${base_url}.param"; then

        warn "Failed to download .param from this source."
        rm -f -- "$param_tmp" "$bin_tmp"
        return 1
    fi

    if ! curl \
        --fail \
        --location \
        --silent \
        --show-error \
        --retry 2 \
        --retry-delay 1 \
        --retry-connrefused \
        --connect-timeout 20 \
        --max-time 300 \
        --output "$bin_tmp" \
        "${base_url}.bin"; then

        warn "Failed to download .bin from this source."
        rm -f -- "$param_tmp" "$bin_tmp"
        return 1
    fi

    if ! validate_download_pair "$param_tmp" "$bin_tmp"; then
        warn "Downloaded files failed validation."
        rm -f -- "$param_tmp" "$bin_tmp"
        return 1
    fi

    # Publish only after both files have passed validation.
    mv -f -- "$param_tmp" "$PARAM_PATH"
    mv -f -- "$bin_tmp" "$BIN_PATH"

    log "Model downloaded and validated successfully."
    return 0
}

SOURCE_BASES=(
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/${MODEL_NAME}"
    "https://raw.githubusercontent.com/xinntao/Real-ESRGAN-ncnn-vulkan/master/models/${MODEL_NAME}"
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-animevideov3-x2"
    "https://raw.githubusercontent.com/xinntao/Real-ESRGAN-ncnn-vulkan/master/models/realesr-animevideov3-x2"
)

for source in "${SOURCE_BASES[@]}"; do
    if download_from_source "$source"; then
        log "PARAM size: $(stat -c '%s' "$PARAM_PATH") bytes"
        log "BIN size:   $(stat -c '%s' "$BIN_PATH") bytes"
        exit 0
    fi
done

warn "All pretrained model sources failed."

# --------------------------------------------------------------------
# 5. Optional synthetic fallback
# --------------------------------------------------------------------

if [[ "$ALLOW_SYNTHETIC_FALLBACK" == "1" ]]; then
    warn "Synthetic fallback is explicitly enabled."
    warn "This is NOT the original pretrained RealESRGAN model."

    [[ -x "$GENERATOR_BIN" ]] ||
        die "Generator is missing or not executable: $GENERATOR_BIN"

    FALLBACK_DIR="${TMP_DIR}/fallback"
    mkdir -p -- "$FALLBACK_DIR"

    "$GENERATOR_BIN" \
        "$MODEL_NAME" \
        "$SCALE" \
        "$FALLBACK_DIR" ||
        die "Synthetic model generation failed."

    FALLBACK_PARAM="${FALLBACK_DIR}/${MODEL_NAME}.param"
    FALLBACK_BIN="${FALLBACK_DIR}/${MODEL_NAME}.bin"

    if ! validate_synthetic_pair "$FALLBACK_PARAM" "$FALLBACK_BIN"; then
        die "Generated fallback model failed validation."
    fi

    mv -f -- "$FALLBACK_PARAM" "$PARAM_PATH"
    mv -f -- "$FALLBACK_BIN" "$BIN_PATH"

    warn "Synthetic fallback generated successfully."
    warn "Its output quality is not equivalent to pretrained RealESRGAN."

    exit 0
fi

# --------------------------------------------------------------------
# 6. Fail the build instead of silently substituting another model
# --------------------------------------------------------------------

die "Unable to obtain a valid pretrained model.
Check network connectivity, model URLs, and file availability.
No synthetic fallback was generated."