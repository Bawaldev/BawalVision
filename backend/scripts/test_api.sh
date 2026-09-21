#!/usr/bin/env bash
# ==============================================================================
# BawalVision Backend API & Session Automated Verification Suite
# OpenCV AI Competition 2026 Validation Script
# ==============================================================================

set -eo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080/api/v1}"
CACHE_DIR=".cache_trash"
COOKIE_JAR="${CACHE_DIR}/cookies.txt"

# Color Codes for Competition Logging
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

mkdir -p "${CACHE_DIR}"

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}  BawalVision Backend API & Session Test Runner       ${NC}"
echo -e "${BLUE}  Target: ${BASE_URL}                                 ${NC}"
echo -e "${BLUE}======================================================${NC}\n"

# ------------------------------------------------------------------------------
# STEP 1: USER REGISTRATION
# ------------------------------------------------------------------------------
TEST_EMAIL="bench-$(date +%s%N)@bawalvision.ai"
TEST_PASS="BenchPass2026!"

echo -e "${YELLOW}[1/4] Testing User Registration...${NC}"
REG_RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/auth/register" \
  -H "Content-Type: application/json" \
  -d "{
    \"name\": \"Benchmark Agent\",
    \"email\": \"${TEST_EMAIL}\",
    \"password\": \"${TEST_PASS}\"
  }")

REG_BODY=$(echo "${REG_RESP}" | head -n -1)
REG_CODE=$(echo "${REG_RESP}" | tail -n 1)

if [ "${REG_CODE}" -eq 201 ]; then
  echo -e "${GREEN}✓ Registration Passed (HTTP 201)${NC}: ${REG_BODY}"
else
  echo -e "${RED}✗ Registration Failed (HTTP ${REG_CODE})${NC}: ${REG_BODY}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 2: USER LOGIN & ENCRYPTED SESSION COOKIE
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[2/4] Testing Login & Session Cookie Extraction...${NC}"
LOGIN_HEADERS=$(curl -s -i -X POST "${BASE_URL}/auth/login" \
  -H "Content-Type: application/json" \
  -d "{
    \"email\": \"${TEST_EMAIL}\",
    \"password\": \"${TEST_PASS}\"
  }" \
  -c "${COOKIE_JAR}")

if echo "${LOGIN_HEADERS}" | grep -q "Set-Cookie: session="; then
  echo -e "${GREEN}✓ Session Cookie Received & Saved to ${COOKIE_JAR}${NC}"
  echo -e "  Extracted Cookie Header:"
  echo "${LOGIN_HEADERS}" | grep "Set-Cookie:" | sed 's/^/    /'
else
  echo -e "${RED}✗ Session Cookie Missing in Login Response!${NC}"
  echo "${LOGIN_HEADERS}"
  exit 1
fi

# ------------------------------------------------------------------------------
# STEP 3: EDGE TELEMETRY INGRESS (ESP32-S3 SIMULATION)
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[3/4] Testing ESP32-S3 Edge Telemetry Ingress...${NC}"
TELEMETRY_RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/telemetry" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "ESP32-S3-BENCH-01",
    "device_name": "Automated Benchmark Pod",
    "firmware_version": "1.0.4-bench",
    "device_core": 2,
    "temperature": 4.35,
    "humidity": 66.80,
    "gas_voc": 110.20,
    "pressure": 1013.25,
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
# STEP 4: FETCH INSPECTION RECORDS WITH SESSION COOKIE
# ------------------------------------------------------------------------------
echo -e "\n${YELLOW}[4/4] Testing Query Records using Active Session Cookie...${NC}"
RECORDS_RESP=$(curl -s -w "\n%{http_code}" -X GET "${BASE_URL}/records" \
  -b "${COOKIE_JAR}")

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

if [ "${CAM_CODE}" -eq 201 ]; then
  echo -e "${GREEN}✓ Camera Frame Ingested (HTTP 201)${NC}: ${CAM_BODY}"
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

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}  All Backend, Camera & Session Tests Passed! (100%)  ${NC}"
echo -e "${GREEN}======================================================${NC}"


