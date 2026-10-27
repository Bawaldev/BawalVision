#!/usr/bin/env bash
# ==============================================================================
# BawalVision AWS Lambda (Edge-First Serverless) 100% Free Tier Deployment
#
# AWS Lambda Always Free Tier:
# - 1,000,000 free requests per month FOREVER
# - 3,200,000 seconds of compute time per month FOREVER
# - $0.00 / month cost when idle (scales to zero)
#
# Usage:
#   AWS_REGION=ap-southeast-1 ./deploy/deploy_lambda.sh
# ==============================================================================
set -euo pipefail

AWS_REGION="${AWS_REGION:-ap-southeast-1}"
FUNCTION_NAME="${FUNCTION_NAME:-bawal-vision-lambda}"
ECR_REPO_NAME="${ECR_REPO_NAME:-bawal-vision-lambda}"
AUTH_TOKEN="${BAWAL_LAMBDA_AUTH_TOKEN:-}"

if [[ ${#AUTH_TOKEN} -lt 32 ]]; then
    echo "[ERROR] BAWAL_LAMBDA_AUTH_TOKEN must be at least 32 characters." >&2
    echo "        Generate one with: openssl rand -hex 32" >&2
    exit 1
fi

LAMBDA_ENV="Variables={MODEL_PATH=/var/task/models/yolov8s_int8.onnx,OMP_NUM_THREADS=2,OPENCV_LOG_LEVEL=ERROR,ENFORCE_HTTP_AUTH=1,BAWAL_LAMBDA_AUTH_TOKEN=${AUTH_TOKEN}}"

echo "=== [1/5] Checking AWS CLI Authentication ==="
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
echo "Authenticated with AWS Account: ${ACCOUNT_ID} (Region: ${AWS_REGION})"

echo "=== [2/5] Creating / Verifying ECR Repository ==="
aws ecr describe-repositories --repository-names "${ECR_REPO_NAME}" --region "${AWS_REGION}" >/dev/null 2>&1 || \
    aws ecr create-repository --repository-name "${ECR_REPO_NAME}" --region "${AWS_REGION}"

ECR_URI="${ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com/${ECR_REPO_NAME}:latest"

echo "=== [3/5] Building Lambda Container Image ==="
aws ecr get-login-password --region "${AWS_REGION}" | docker login --username AWS --password-stdin "${ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com"
docker build -f deploy/Dockerfile.lambda -t "${ECR_REPO_NAME}:latest" .
docker tag "${ECR_REPO_NAME}:latest" "${ECR_URI}"

echo "=== [4/5] Pushing Container to Amazon ECR ==="
docker push "${ECR_URI}"

echo "=== [5/5] Deploying / Updating AWS Lambda Function ==="
ROLE_ARN=$(aws iam get-role --role-name lambda-basic-role --query Role.Arn --output text 2>/dev/null || true)
if [ -z "${ROLE_ARN}" ]; then
    echo "Creating IAM Role lambda-basic-role..."
    aws iam create-role --role-name lambda-basic-role \
        --assume-role-policy-document '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}' >/dev/null
    aws iam attach-role-policy --role-name lambda-basic-role --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
    sleep 5
    ROLE_ARN=$(aws iam get-role --role-name lambda-basic-role --query Role.Arn --output text)
fi

if aws lambda get-function --function-name "${FUNCTION_NAME}" --region "${AWS_REGION}" >/dev/null 2>&1; then
    echo "Updating existing Lambda function code..."
    aws lambda update-function-code --function-name "${FUNCTION_NAME}" --image-uri "${ECR_URI}" --region "${AWS_REGION}" >/dev/null
    aws lambda wait function-updated --function-name "${FUNCTION_NAME}" --region "${AWS_REGION}"
    aws lambda update-function-configuration \
        --function-name "${FUNCTION_NAME}" \
        --environment "${LAMBDA_ENV}" \
        --region "${AWS_REGION}" >/dev/null
    aws lambda wait function-updated --function-name "${FUNCTION_NAME}" --region "${AWS_REGION}"
    aws lambda update-function-url-config \
        --function-name "${FUNCTION_NAME}" \
        --auth-type NONE \
        --cors '{"AllowOrigins":["*"],"AllowMethods":["*"],"AllowHeaders":["*"],"ExposeHeaders":["*"],"MaxAge":86400}' \
        --region "${AWS_REGION}" >/dev/null 2>&1 || true
else
    echo "Creating new Lambda function with Function URL..."
    aws lambda create-function \
        --function-name "${FUNCTION_NAME}" \
        --package-type Image \
        --code ImageUri="${ECR_URI}" \
        --role "${ROLE_ARN}" \
        --timeout 15 \
        --memory-size 512 \
        --environment "${LAMBDA_ENV}" \
        --region "${AWS_REGION}" >/dev/null

    aws lambda create-function-url-config \
        --function-name "${FUNCTION_NAME}" \
        --auth-type NONE \
        --cors '{"AllowOrigins":["*"],"AllowMethods":["*"],"AllowHeaders":["*"],"ExposeHeaders":["*"],"MaxAge":86400}' \
        --region "${AWS_REGION}" >/dev/null

    aws lambda add-permission \
        --function-name "${FUNCTION_NAME}" \
        --statement-id FunctionURLAllowPublicAccess \
        --action lambda:InvokeFunctionUrl \
        --principal "*" \
        --function-url-auth-type NONE \
        --region "${AWS_REGION}" >/dev/null
fi

FUNC_URL=$(aws lambda get-function-url-config --function-name "${FUNCTION_NAME}" --region "${AWS_REGION}" --query FunctionUrl --output text)

echo "=========================================================================="
echo "✓ AWS Lambda Successfully Deployed (100% Free Tier Always Active)"
echo "  Function URL: ${FUNC_URL}"
echo ""
echo "Next step for Vercel Frontend:"
echo "Set environment variable in Vercel Dashboard:"
echo "  AWS_LAMBDA_URL=${FUNC_URL}"
echo "=========================================================================="

