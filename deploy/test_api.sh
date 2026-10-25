#!/usr/bin/env bash
# ==============================================================================
# BawalVision Crow C++ Backend API Verification Suite
# OpenCV AI Competition 2026 Validation Script
#
# Targets the unified C++ binary (services/build/bin/bawal_backend). Run the
# server first, e.g.:
#   CROW_PORT=18081 MODEL_PATH=ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx \
#     ./services/build/bin/bawal_backend
#   BASE_URL=http://127.0.0.1:18081/api/v1 ./deploy/test_api.sh
# ==============================================================================

set -eo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080/api/v1}"
CACHE_DIR="${CACHE_DIR:-.cache_trash}"
DEVICE_ID="ESP32-S3-CURL-TEST"
FRAME_FILE="${CACHE_DIR}/crow_test_frame.jpg"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

mkdir -p "${CACHE_DIR}"

curl_json() {
  local method="$1"
  local url="$2"
  shift 2
  curl -sS --max-time 15 -X "${method}" "${url}" "$@"
}

assert_contains() {
  local payload="$1"
  local needle="$2"
  local label="$3"
  if [[ "${payload}" != *"${needle}"* ]]; then
    echo -e "${RED}✗ ${label}${NC}"
    echo "  Missing: ${needle}"
    echo "  Body: ${payload:0:600}"
    exit 1
  fi
  echo -e "${GREEN}✓ ${label}${NC}"
}

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}  BawalVision Crow C++ API Test Runner               ${NC}"
echo -e "${BLUE}  Target: ${BASE_URL}                                 ${NC}"
echo -e "${BLUE}======================================================${NC}\n"

# ------------------------------------------------------------------------------
# Test frame: prefer a real video frame so YOLO actually runs; fall back to a
# tiny embedded JPEG when the example asset/ffmpeg is unavailable.
# ------------------------------------------------------------------------------
EXAMPLE_VIDEO="examples/banana/banana_sample_one.mp4"
if command -v ffmpeg >/dev/null 2>&1 && [[ -f "${EXAMPLE_VIDEO}" ]]; then
  ffmpeg -v error -y -ss 2 -i "${EXAMPLE_VIDEO}" -frames:v 1 -q:v 2 "${FRAME_FILE}"
else
  echo '/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAAPAA8BAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPx8Af//Z' \
    | base64 -d > "${FRAME_FILE}"
fi
echo -e "[INFO] Test frame: ${FRAME_FILE} ($(wc -c < "${FRAME_FILE}") bytes)\n"

# ------------------------------------------------------------------------------
# STEP 1: HEALTH
# ------------------------------------------------------------------------------
echo -e "${YELLOW}[1/8] GET /health${NC}"
HEALTH=$(curl_json GET "${BASE_URL}/health")
assert_contains "${HEALTH}" '"status":"ok"' "Crow health endpoint"
assert_contains "${HEALTH}" '"service":"bawal-crow-backend"' "Crow service identity"

# ------------------------------------------------------------------------------
# STEP 2: CAMERA ACTIVE (before ingress)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[2/8] GET /camera/active${NC}"
ACTIVE_BEFORE=$(curl_json GET "${BASE_URL}/camera/active")
assert_contains "${ACTIVE_BEFORE}" '"active"' "Camera active snapshot"

# ------------------------------------------------------------------------------
# STEP 3: FRAME INGRESS (raw JPEG bytes + JSON/base64 compatibility)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[3/8] POST /camera/frame (raw JPEG -> POSIX SHM)${NC}"
FRAME_RESP=$(curl_json POST "${BASE_URL}/camera/frame" \
  -H "Content-Type: image/jpeg" \
  -H "X-Device-ID: ${DEVICE_ID}" \
  -H "X-Frame-ID: 1001" \
  --data-binary "@${FRAME_FILE}")
assert_contains "${FRAME_RESP}" '"status":"success"' "Raw JPEG frame accepted"
assert_contains "${FRAME_RESP}" '"shm_write_sequence"' "Frame published to POSIX SHM"

FRAME_B64=$(base64 -w0 "${FRAME_FILE}")
JSON_RESP=$(curl_json POST "${BASE_URL}/camera/frame" \
  -H "Content-Type: application/json" \
  -d "{\"device_id\":\"${DEVICE_ID}\",\"frame_id\":1002,\"width\":640,\"height\":480,\"format\":0,\"image_base64\":\"${FRAME_B64}\"}")
assert_contains "${JSON_RESP}" '"status":"success"' "JSON/base64 frame accepted"

# Give the vision thread time to consume the newest SHM frame. Keep this below
# the 1.5 s camera staleness window so the active camera check stays reliable.
sleep 1

# ------------------------------------------------------------------------------
# STEP 4: CAMERA ACTIVE (after ingress)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[4/8] GET /camera/active (after ingress)${NC}"
ACTIVE_AFTER=$(curl_json GET "${BASE_URL}/camera/active")
assert_contains "${ACTIVE_AFTER}" '"active":true' "Camera marked active"
if [[ "${ACTIVE_AFTER}" == *"\"device_id\":\"${DEVICE_ID}\""* ]]; then
  echo -e "${GREEN}✓ Device id propagated${NC}"
elif [[ "${ACTIVE_AFTER}" == *"\"device_id\":\"ESP32"* ]]; then
  # A physical ESP32-S3 streaming at 10-20 FPS overwrites the shared camera
  # slot between the POST and this GET; accept the live hardware node.
  echo -e "${GREEN}✓ Device id propagated (live ESP32 node is streaming concurrently)${NC}"
else
  echo -e "${RED}✗ Device id propagated${NC}"
  echo "  Missing: \"device_id\":\"${DEVICE_ID}\""
  echo "  Body: ${ACTIVE_AFTER:0:600}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 5: MJPEG STREAM + RAW FRAME
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[5/8] GET /camera/stream + /camera/latest/<device>/raw${NC}"
STREAM_HEADERS="${CACHE_DIR}/crow_stream_headers.txt"
STREAM_CODE=$(curl -sS --max-time 15 -D "${STREAM_HEADERS}" -o "${CACHE_DIR}/crow_stream_body.bin" \
  -w '%{http_code}' "${BASE_URL}/camera/stream")
if [[ "${STREAM_CODE}" != "200" ]] || ! grep -qi 'multipart/x-mixed-replace' "${STREAM_HEADERS}"; then
  echo -e "${RED}✗ MJPEG stream endpoint${NC} (HTTP ${STREAM_CODE})"
  exit 1
fi
echo -e "${GREEN}✓ MJPEG stream endpoint (HTTP 200, multipart boundary)${NC}"

RAW_HEADERS="${CACHE_DIR}/crow_raw_headers.txt"
RAW_CODE=$(curl -sS --max-time 15 -D "${RAW_HEADERS}" -o "${CACHE_DIR}/crow_raw_frame.jpg" \
  -w '%{http_code}' "${BASE_URL}/camera/latest/${DEVICE_ID}/raw")
if [[ "${RAW_CODE}" != "200" ]]; then
  # A live ESP32-S3 node may have replaced the test device in the shared slot.
  RAW_CODE=$(curl -sS --max-time 15 -D "${RAW_HEADERS}" -o "${CACHE_DIR}/crow_raw_frame.jpg" \
    -w '%{http_code}' "${BASE_URL}/camera/latest/raw")
fi
if [[ "${RAW_CODE}" != "200" ]] || ! grep -qi 'content-type: image/jpeg' "${RAW_HEADERS}"; then
  echo -e "${RED}✗ Raw frame endpoint${NC} (HTTP ${RAW_CODE})"
  exit 1
fi
echo -e "${GREEN}✓ Raw frame endpoint (HTTP 200, image/jpeg)${NC}"

# ------------------------------------------------------------------------------
# STEP 6: TELEMETRY (in-memory, no Postgres)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[6/8] GET /telemetry/latest${NC}"
TELEMETRY=$(curl_json GET "${BASE_URL}/telemetry/latest")
assert_contains "${TELEMETRY}" '"telemetry_struct":"TelemetryIngressPayload"' "Telemetry payload shape"
assert_contains "${TELEMETRY}" '"vision_ready":true' "Vision thread ready"

# ------------------------------------------------------------------------------
# STEP 7: LATEST INSPECTION (YOLO detections from memory)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[7/8] GET /inspections/latest${NC}"
INSPECTION=$(curl_json GET "${BASE_URL}/inspections/latest")
assert_contains "${INSPECTION}" '"inspection_struct":"InspectionRecord"' "Inspection payload shape"
assert_contains "${INSPECTION}" '"detections":[' "Detection SoA serialized"
assert_contains "${INSPECTION}" '"engine_pipeline"' "YOLO engine metadata"

echo -e "\n${BLUE}Inspection snapshot:${NC}"
echo "${INSPECTION:0:900}"

# ------------------------------------------------------------------------------
# STEP 8: MARKDOWN -> PDF REPORT (portable C++ engine, no TeX required)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[8/8] POST /reports/pdf (Markdown -> PDF)${NC}"
REPORT_HEADERS="${CACHE_DIR}/crow_report_headers.txt"
REPORT_BODY="${CACHE_DIR}/crow_report.pdf"
REPORT_CODE=$(curl -sS --max-time 30 -D "${REPORT_HEADERS}" -o "${REPORT_BODY}" \
  -w '%{http_code}' -X POST "${BASE_URL}/reports/pdf" \
  -H "Content-Type: application/json" \
  -d '{"markdown":"# BawalVision API Test\n\n| Metric | Value |\n| --- | --- |\n| Verdict | ACCEPTED |\n| PDF Engine | C++ |\n","title":"BawalVision API Test","filename":"api-test.pdf"}')
if [[ "${REPORT_CODE}" != "200" ]] || ! head -c 4 "${REPORT_BODY}" | grep -q '%PDF'; then
  echo -e "${RED}✗ Markdown -> PDF report endpoint${NC} (HTTP ${REPORT_CODE})"
  exit 1
fi
if ! grep -qi 'x-report-format: pdf' "${REPORT_HEADERS}" || \
   ! grep -qi 'content-type: application/pdf' "${REPORT_HEADERS}"; then
  echo -e "${RED}✗ PDF report headers${NC}"
  exit 1
fi
echo -e "${GREEN}✓ Markdown -> PDF report endpoint (HTTP 200, %PDF artifact)${NC}"

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}  All Crow C++ Backend API Tests Passed!             ${NC}"
echo -e "${GREEN}======================================================${NC}"
