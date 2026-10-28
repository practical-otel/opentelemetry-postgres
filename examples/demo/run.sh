#!/usr/bin/env bash
# Build, start, drive and verify the demo. Leaves nothing running unless KEEP=1.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf out && mkdir -p out && chmod 777 out
cleanup() { [ -n "${KEEP:-}" ] || docker compose down -v --remove-orphans >/dev/null 2>&1; }
trap cleanup EXIT
docker compose up -d --build --wait
python3 verify.py
