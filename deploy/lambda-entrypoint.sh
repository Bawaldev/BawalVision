#!/usr/bin/env bash
set -euo pipefail

# AWS Lambda sets AWS_LAMBDA_RUNTIME_API for real invocations. When it is not
# set we are running locally (docker run), so start the Runtime Interface
# Emulator and point the bootstrap at its internal runtime API port.
if [[ -z "${AWS_LAMBDA_RUNTIME_API:-}" ]]; then
    export AWS_LAMBDA_RUNTIME_API=127.0.0.1:9001
    exec /usr/local/bin/aws-lambda-rie /var/task/bootstrap
fi

exec /var/task/bootstrap
