#!/usr/bin/env bash
# Build, start, drive and verify the managed-database demo. Leaves nothing running unless KEEP=1.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf out pglogs && mkdir -p out pglogs && chmod 777 out pglogs
cleanup() { [ -n "${KEEP:-}" ] || docker compose down -v --remove-orphans >/dev/null 2>&1; }
trap cleanup EXIT
docker compose up -d --build --wait
MODE=log python3 ../demo/verify.py
