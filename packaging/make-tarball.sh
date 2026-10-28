#!/bin/sh
# Run inside a build container after `make`: packs the library and install.sh.
#   make-tarball.sh <pg-major> <output-dir>
set -eu
PG_MAJOR="$1"; OUT="$2"
VERSION="$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" opentelemetry_postgres.control)"
ARCH="$(uname -m)"; [ "$ARCH" = arm64 ] && ARCH=aarch64
if ldd --version 2>&1 | grep -qi musl; then LIBC=musl; else LIBC=glibc; fi
NAME="opentelemetry-postgres-${VERSION}-pg${PG_MAJOR}-linux-${ARCH}-${LIBC}"

mkdir -p "$OUT/$NAME"
cp opentelemetry_postgres.so opentelemetry_postgres.control sql/opentelemetry_postgres--*.sql \
   install.sh LICENSE README.md "$OUT/$NAME/"
strip --strip-unneeded "$OUT/$NAME/opentelemetry_postgres.so" 2>/dev/null || true
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
rm -rf "$OUT/$NAME"

# Report what the binary needs from the system, so portability regressions are visible.
echo "== $NAME.tar.gz"
if [ "$LIBC" = glibc ] && command -v objdump >/dev/null 2>&1; then
  tar -xzOf "$OUT/$NAME.tar.gz" "$NAME/opentelemetry_postgres.so" > /tmp/probe.so
  echo "newest glibc symbol required: $(objdump -T /tmp/probe.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n1)"
fi
