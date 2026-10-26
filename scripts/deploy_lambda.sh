#!/usr/bin/env bash
set -euo pipefail

# Build, push, and deploy the BawalVision Lambda container image.
#
# Required tools: docker, aws (v2), sam
# Required AWS permissions: ECR push, CloudFormation, IAM role creation,
# API Gateway, Lambda, CloudWatch Logs.
#
# Optional environment:
#   AWS_REGION       (default: us-east-1)
#   STACK_NAME       (default: bawalvision-lambda)
#   ECR_REPOSITORY   (default: bawalvision-lambda)
#   IMAGE_TAG        (default: git short SHA or timestamp)
#   STAGE_NAME       (default: prod)
#   MEMORY_SIZE      (default: 3008)
#   LAMBDA_TIMEOUT   (default: 29)
#   MAX_CONCURRENCY  (default: 5)
#   LAMBDA_PREWARM   (default: 0)
#   BAWAL_LAMBDA_AUTH_TOKEN (required, 32+ characters)
#   DOCKER_PLATFORM  (default: linux/amd64, matching the SAM architecture)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-}}"
if [[ -z "$REGION" && -f "$ROOT_DIR/.env" ]]; then
    REGION="$(grep -E "execute-api|lambda-url" "$ROOT_DIR/.env" | grep -oE '[a-z]{2}-[a-z]+-[0-9]' | head -n 1 || true)"
fi
REGION="${REGION:-us-east-1}"
STACK_NAME="${STACK_NAME:-bawalvision-lambda}"
ECR_REPOSITORY="${ECR_REPOSITORY:-bawalvision-lambda}"
IMAGE_TAG="${IMAGE_TAG:-$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || date +%Y%m%d%H%M%S)}"
STAGE_NAME="${STAGE_NAME:-prod}"
MEMORY_SIZE="${MEMORY_SIZE:-3008}"
LAMBDA_TIMEOUT="${LAMBDA_TIMEOUT:-29}"
MAX_CONCURRENCY="${MAX_CONCURRENCY:-5}"
LAMBDA_PREWARM="${LAMBDA_PREWARM:-0}"
LAMBDA_AUTH_MODE="${LAMBDA_AUTH_MODE:-NONE}"
LAMBDA_AUTH_TOKEN="${BAWAL_LAMBDA_AUTH_TOKEN:-}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"

for tool in docker aws sam; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] Required tool not found: $tool" >&2
        exit 1
    fi
done

if [[ ${#LAMBDA_AUTH_TOKEN} -lt 32 ]]; then
    echo "[ERROR] BAWAL_LAMBDA_AUTH_TOKEN must be at least 32 characters." >&2
    exit 1
fi

ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text)"
ECR_REGISTRY="${ACCOUNT_ID}.dkr.ecr.${REGION}.amazonaws.com"
IMAGE_URI="${ECR_REGISTRY}/${ECR_REPOSITORY}:${IMAGE_TAG}"

echo "[INFO] Region:      ${REGION}"
echo "[INFO] Stack:       ${STACK_NAME}"
echo "[INFO] Image:       ${IMAGE_URI}"
echo "[INFO] Platform:    ${DOCKER_PLATFORM}"

if ! aws ecr describe-repositories --region "$REGION" \
        --repository-names "$ECR_REPOSITORY" >/dev/null 2>&1; then
    aws ecr create-repository --region "$REGION" \
        --repository-name "$ECR_REPOSITORY" \
        --image-scanning-configuration scanOnPush=true \
        --encryption-configuration encryptionType=AES256 >/dev/null
    echo "[INFO] Created ECR repository ${ECR_REPOSITORY}"
fi

aws ecr get-login-password --region "$REGION" |
    docker login --username AWS --password-stdin "$ECR_REGISTRY"

echo "[INFO] Building container image..."
docker build \
    --platform "$DOCKER_PLATFORM" \
    --file "$ROOT_DIR/deploy/Dockerfile.lambda" \
    --tag "$IMAGE_URI" \
    "$ROOT_DIR"

echo "[INFO] Pushing container image..."
docker push "$IMAGE_URI"

echo "[INFO] Deploying SAM stack..."
sam deploy \
    --template-file "$ROOT_DIR/aws-infra/template.yaml" \
    --stack-name "$STACK_NAME" \
    --region "$REGION" \
    --capabilities CAPABILITY_IAM \
    --resolve-s3 \
    --image-repository "${ECR_REGISTRY}/${ECR_REPOSITORY}" \
    --no-confirm-changeset \
    --parameter-overrides \
        "ImageUri=${IMAGE_URI}" \
        "StageName=${STAGE_NAME}" \
        "MemorySize=${MEMORY_SIZE}" \
        "Timeout=${LAMBDA_TIMEOUT}" \
        "MaxConcurrency=${MAX_CONCURRENCY}" \
        "LambdaPrewarm=${LAMBDA_PREWARM}" \
        "LambdaAuthMode=${LAMBDA_AUTH_MODE}" \
        "AuthToken=${LAMBDA_AUTH_TOKEN}"

API_ENDPOINT="$(aws cloudformation describe-stacks --region "$REGION" \
    --stack-name "$STACK_NAME" \
    --query "Stacks[0].Outputs[?OutputKey=='ApiEndpoint'].OutputValue" \
    --output text)"

echo "[SUCCESS] Deployed ${STACK_NAME}"
echo "[INFO] Snapshot: ${API_ENDPOINT}/api/v1/lambda/inspect"
echo "[INFO] Batch:    ${API_ENDPOINT}/api/v1/lambda/batch"
echo "[INFO] PDF:      ${API_ENDPOINT}/api/v1/reports/pdf"
echo "[INFO] PDF alias: ${API_ENDPOINT}/api/v1/lambda/report"
echo "[INFO] API Gateway routes require AWS_IAM SigV4 authorization."
echo "[INFO] Function URL/HTTP invocations require X-Bawal-Token: set BAWAL_LAMBDA_AUTH_TOKEN in the proxy."
