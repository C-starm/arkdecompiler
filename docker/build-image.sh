#!/usr/bin/env bash
# Build the arkdecompiler Docker image (compiles xabc + toolchain).
#
# This pulls the full OpenHarmony Ark toolchain (~tens of GB) and compiles it,
# so the FIRST build is long (often hours, network-bound on gitee). Subsequent
# builds reuse Docker layer cache.
#
# Usage:
#   sudo ./docker/build-image.sh            # build, tag arkdecompiler:latest
#   sudo ./docker/build-image.sh mytag      # custom tag
#
# After it succeeds, point the MCP server at it:
#   ARKDEC_BACKEND=docker ARKDEC_DOCKER_IMAGE=arkdecompiler:latest
set -euo pipefail

TAG="${1:-arkdecompiler:latest}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CTX="$(cd "$HERE/.." && pwd)"

echo "[*] Building image '$TAG'"
echo "[*] Build context: $CTX"
echo "[*] WARNING: first build downloads the OpenHarmony toolchain (tens of GB) and"
echo "    compiles it. This can take a very long time. Keep the machine awake."

# Build. The Dockerfile clones arkdecompiler from GitHub itself, so the context
# is only needed for the Dockerfile; we still pass CTX so local patch overrides
# (if any) are available to future iterations.
docker build -f "$HERE/Dockerfile" -t "$TAG" "$CTX"

echo
echo "[+] Image built: $TAG"
echo "[+] Verify xabc exists inside the image:"
echo "    docker run --rm $TAG bash -lc 'ls -l /root/harmonyos/arkdecompiler/out/arkcompiler/common/xabc'"
