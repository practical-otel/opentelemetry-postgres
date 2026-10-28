#!/usr/bin/env bash
# Sideload the extension into the stock postgres image and prove it loaded.
#   ./run.sh                  downloads the release for $PG_MAJOR (default 17)
#   DIST=../../dist ./run.sh  uses locally built tarballs instead (see packaging/)
set -euo pipefail
cd "$(dirname "$0")"
export PG_MAJOR="${PG_MAJOR:-17}"
rm -rf ext && mkdir ext
if [ -n "${DIST:-}" ]; then
  tar -xzf "$DIST"/*-pg"$PG_MAJOR"-linux-"$(uname -m)"-glibc.tar.gz -C ext --strip-components=1
else
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  base="https://github.com/practical-otel/opentelemetry-postgres/releases/latest/download"
  curl -fsSL "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
  f=$(grep -E "pg${PG_MAJOR}-linux-$(uname -m)-glibc\.tar\.gz$" "$tmp/SHA256SUMS" | awk '{print $2}')
  curl -fsSL "$base/$f" -o "$tmp/$f"; (cd "$tmp" && grep " $f\$" SHA256SUMS | sha256sum -c -)
  tar -xzf "$tmp/$f" -C ext --strip-components=1
fi
trap 'docker compose down -v >/dev/null 2>&1' EXIT
docker compose up -d --wait
docker compose exec -T postgres psql -U postgres -Atc "CREATE EXTENSION opentelemetry_postgres"
docker compose exec -T postgres psql -U postgres -Atc "SHOW shared_preload_libraries"
docker compose exec -T postgres psql -U postgres -Atc "SELECT 'otel_stats: ' || row(s.*)::text FROM otel_stats() s"
docker compose exec -T postgres sh -c "ps -eo args | grep -F 'opentelemetry-postgres exporter' | grep -v grep"
echo "image: $(docker compose config --images)  (unmodified official image)"
