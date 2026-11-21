#!/usr/bin/env bash
# Usage : build.sh <env>   (ex. esp32dev, esp32dev_diag, cyd) -- compile mcz-maestro-ble dans Docker. Aucun upload.
set -euo pipefail
ENV_NAME="${1:?usage: build.sh <env>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"   # racine du depot
IMAGE=mcz-platformio:6.1.16
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build --platform linux/arm64 -t "$IMAGE" "$HERE"
docker run --rm -v "$REPO:/project" -v pio-cache:/root/.platformio -w /project "$IMAGE" pio run -e "$ENV_NAME"
