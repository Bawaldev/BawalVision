#!/usr/bin/env bash
# ==============================================================================
# BawalVision Backend API & Session Automated Verification Suite
# OpenCV AI Competition 2026 Validation Script
# ==============================================================================

set -eo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080/api/v1}"
CACHE_DIR=".cache_trash"

# Color Codes for Competition Logging
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

mkdir -p "${CACHE_DIR}"

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}  BawalVision Backend Open Web API Test Runner        ${NC}"
echo -e "${BLUE}  Target: ${BASE_URL}                                 ${NC}"
echo -e "${BLUE}======================================================${NC}\n"

# ------------------------------------------------------------------------------
# STEP 1: EDGE TELEMETRY INGRESS (ESP32-S3 SIMULATION)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[1/3] Testing ESP32-S3 Edge Telemetry Ingress...${NC}"
TELEMETRY_RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/telemetry" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32-S3-BENCH-01",
    "device_name": "Automated Benchmark Pod",
    "firmware_version": "1.0.4-bench",
    "device_core": 2,
    "cpu_temperature": 39.20,
    "temperature": 28.50,
    "cpu_load": 15,
    "timestamp": 1758412800
  }')

TELEMETRY_BODY=$(echo "${TELEMETRY_RESP}" | head -n -1)
TELEMETRY_CODE=$(echo "${TELEMETRY_RESP}" | tail -n 1)

if [ "${TELEMETRY_CODE}" -eq 201 ]; then
  echo -e "${GREEN}✓ Ingress Successful (HTTP 201)${NC}: ${TELEMETRY_BODY}"
else
  echo -e "${RED}✗ Ingress Failed (HTTP ${TELEMETRY_CODE})${NC}: ${TELEMETRY_BODY}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 2: FETCH INSPECTION RECORDS (OPEN SOURCE WEB ACCESS)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[2/3] Testing Query Records (Open Web Access)...${NC}"
RECORDS_RESP=$(curl -s -w "\n%{http_code}" -X GET "${BASE_URL}/records")

RECORDS_BODY=$(echo "${RECORDS_RESP}" | head -n -1)
RECORDS_CODE=$(echo "${RECORDS_RESP}" | tail -n 1)

if [ "${RECORDS_CODE}" -eq 200 ]; then
  echo -e "${GREEN}✓ Records Query Successful (HTTP 200)${NC}"
  echo -e "  Sample Output: ${RECORDS_BODY:0:160}..."
else
  echo -e "${RED}✗ Records Query Failed (HTTP ${RECORDS_CODE})${NC}: ${RECORDS_BODY}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 5: CAMERA FRAME INGRESS & FAST OPENCV RETRIEVAL
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[5/5] Testing Camera Frame Ingress & In-Memory Retrieval...${NC}"
CAM_RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/camera/frame" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32-S3-BENCH-01",
    "frame_id": 1,
    "width": 320,
    "height": 240,
    "format": 0,
    "buffer_len": 94,
    "timestamp_ms": 1789958045000,
    "image_base64": "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAAPAA8BAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPx8Af//Z"
  }')

CAM_BODY=$(echo "${CAM_RESP}" | head -n -1)
CAM_CODE=$(echo "${CAM_RESP}" | tail -n 1)

if [ "${CAM_CODE}" -eq 200 ] || [ "${CAM_CODE}" -eq 201 ]; then
  echo -e "${GREEN}✓ Camera Frame Ingested (HTTP ${CAM_CODE})${NC}: ${CAM_BODY}"
else
  echo -e "${RED}✗ Camera Frame Ingest Failed (HTTP ${CAM_CODE})${NC}: ${CAM_BODY}"
  exit 1
fi

CAM_GET_RESP=$(curl -s -w "\n%{http_code}" -X GET "${BASE_URL}/camera/latest/ESP32-S3-BENCH-01")
CAM_GET_BODY=$(echo "${CAM_GET_RESP}" | head -n -1)
CAM_GET_CODE=$(echo "${CAM_GET_RESP}" | tail -n 1)

if [ "${CAM_GET_CODE}" -eq 200 ]; then
  echo -e "${GREEN}✓ Latest Camera Frame Retrieved (HTTP 200)${NC}: ${CAM_GET_BODY:0:120}..."
else
  echo -e "${RED}✗ Camera Frame Query Failed (HTTP ${CAM_GET_CODE})${NC}: ${CAM_GET_BODY}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 6: INSPECTION INGRESS WITH FOOD DETECTIONS & PRELOAD VERIFICATION
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[6/6] Testing Vision Inspection Ingress & Food Item Persistence...${NC}"
INSP_RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/inspections" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32-S3-BENCH-01",
    "detections": [
      {
        "food_name": "nasi",
        "similarity_rate": 0.88,
        "weight_grams": 150.0
      },
      {
        "food_name": "ayam",
        "similarity_rate": 0.92,
        "weight_grams": 110.0
      }
    ]
  }')

INSP_BODY=$(echo "${INSP_RESP}" | head -n -1)
INSP_CODE=$(echo "${INSP_RESP}" | tail -n 1)

if [ "${INSP_CODE}" -eq 201 ]; then
  echo -e "${GREEN}✓ Inspection Ingress Successful (HTTP 201)${NC}: ${INSP_BODY}"
else
  echo -e "${RED}✗ Inspection Ingress Failed (HTTP ${INSP_CODE})${NC}: ${INSP_BODY}"
  exit 1
fi

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}  All Backend, Camera & Inspection Tests Passed!     ${NC}"
echo -e "${GREEN}======================================================${NC}"


