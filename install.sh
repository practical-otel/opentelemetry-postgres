#!/bin/sh
# Install opentelemetry-postgres into an existing PostgreSQL (14-17) on Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/practical-otel/opentelemetry-postgres/main/install.sh | sh
#   ./install.sh --pg-config /usr/lib/postgresql/16/bin/pg_config   # or --pg-bin DIR if there is no pg_config
#   ./install.sh --from ./opentelemetry-postgres-0.1.0-pg16-linux-x86_64-glibc   # offline
#   ./install.sh --uninstall
#
# No compiler or server headers are needed: it downloads the prebuilt library that
# matches your PostgreSQL major version, CPU architecture and libc, and copies three
# files into the server's library and extension directories (found via pg_config, or by
# inspecting the server install when pg_config is absent). It does not edit postgresql.conf.
set -eu

REPO="${OTEL_PG_REPO:-practical-otel/opentelemetry-postgres}"
VERSION="${OTEL_PG_VERSION:-latest}"
PG_CONFIG="${PG_CONFIG:-}"
PG_BIN=""
FROM=""
UNINSTALL=0

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --pg-config) PG_CONFIG="${2:?--pg-config needs a path}"; shift 2 ;;
    --pg-bin)    PG_BIN="${2:?--pg-bin needs a directory}"; shift 2 ;;
    --version)   VERSION="${2:?--version needs a value}"; shift 2 ;;
    --from)      FROM="${2:?--from needs a directory or .tar.gz}"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

# ---- locate PostgreSQL -----------------------------------------------------
# Preferred: pg_config. Server-only packages (e.g. Debian's postgresql-N) don't ship it,
# so fall back to finding the server binary and checking the usual directory layouts.
PKGLIBDIR=""; SHAREDIR=""; MAJOR=""

# On Debian/Ubuntu /usr/bin/pg_config can be a wrapper that only says "install
# postgresql-server-dev-NN"; only trust a pg_config that actually answers.
if [ -z "$PG_CONFIG" ] && [ -z "$PG_BIN" ] && command -v pg_config >/dev/null 2>&1 && pg_config --version >/dev/null 2>&1; then
  PG_CONFIG="$(command -v pg_config)"
fi
if [ -z "$PG_CONFIG" ] && [ -z "$PG_BIN" ]; then
  # Several installs? Prefer an unambiguous pg_config, else an unambiguous server.
  found=""; for c in /usr/lib/postgresql/*/bin/pg_config /usr/pgsql-*/bin/pg_config /usr/local/pgsql/bin/pg_config; do
    [ -x "$c" ] && found="$found $c"; done
  set -- $found
  if [ $# -eq 1 ]; then PG_CONFIG="$1"; elif [ $# -gt 1 ]; then
    say "Several PostgreSQL installations found:"; for c in "$@"; do say "  $c"; done
    die "pick one with --pg-config"
  fi
fi

if [ -n "$PG_CONFIG" ]; then
  "$PG_CONFIG" --version >/dev/null 2>&1 || die "$PG_CONFIG does not work (is the PostgreSQL development package installed?)"
  PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
  SHAREDIR="$("$PG_CONFIG" --sharedir)"
  PGVER="$("$PG_CONFIG" --version)"
  MAJOR="$(printf '%s' "$PGVER" | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')"
  WHERE="$PG_CONFIG"
else
  if [ -z "$PG_BIN" ]; then
    found=""; for c in /usr/lib/postgresql/*/bin/postgres /usr/pgsql-*/bin/postgres /usr/local/pgsql/bin/postgres; do
      [ -x "$c" ] && found="$found $(dirname "$c")"; done
    if command -v postgres >/dev/null 2>&1; then found="$found $(dirname "$(command -v postgres)")"; fi
    set -- $(printf '%s\n' $found | sort -u)
    if [ $# -eq 1 ]; then PG_BIN="$1"; elif [ $# -gt 1 ]; then
      say "Several PostgreSQL installations found:"; for c in "$@"; do say "  $c"; done
      die "pick one with --pg-bin DIR (the directory containing the postgres binary)"
    fi
  fi
  [ -n "$PG_BIN" ] && [ -x "$PG_BIN/postgres" ] || die "PostgreSQL not found. Pass --pg-config /path/to/pg_config or --pg-bin /dir/with/postgres"
  PGVER="$("$PG_BIN/postgres" --version)"
  MAJOR="$(printf '%s' "$PGVER" | sed -n 's/.*PostgreSQL) \([0-9][0-9]*\).*/\1/p')"
  [ -n "$MAJOR" ] || die "cannot parse PostgreSQL version from '$PGVER'"
  root="$(dirname "$PG_BIN")"
  for d in "$root/lib/postgresql" "$root/lib" "/usr/lib/postgresql/$MAJOR/lib" "/usr/lib64/pgsql" "/usr/local/lib/postgresql"; do
    [ -f "$d/plpgsql.so" ] && { PKGLIBDIR="$d"; break; }; done
  for d in "$root/share/postgresql" "$root/share" "/usr/share/postgresql/$MAJOR" "/usr/share/pgsql" "/usr/local/share/postgresql"; do
    [ -f "$d/extension/plpgsql.control" ] && { SHAREDIR="$d"; break; }; done
  [ -n "$PKGLIBDIR" ] && [ -n "$SHAREDIR" ] || die "found PostgreSQL $MAJOR in $PG_BIN but not its library/share directories; install your distro's pg_config package (e.g. libpq-dev / postgresql-devel) and pass --pg-config"
  WHERE="$PG_BIN/postgres"
fi
[ -n "$MAJOR" ] || die "cannot determine the PostgreSQL major version"
EXTDIR="$SHAREDIR/extension"

# ---- privileges ------------------------------------------------------------
SUDO=""
if [ ! -w "$PKGLIBDIR" ] || { [ -d "$EXTDIR" ] && [ ! -w "$EXTDIR" ]; }; then
  if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || die "$PKGLIBDIR is not writable; run as root"
    SUDO="sudo"
  fi
fi

if [ "$UNINSTALL" -eq 1 ]; then
  say "Removing opentelemetry_postgres from $PKGLIBDIR and $EXTDIR"
  $SUDO rm -f "$PKGLIBDIR/opentelemetry_postgres.so" "$EXTDIR"/opentelemetry_postgres.control \
              "$EXTDIR"/opentelemetry_postgres--*.sql
  say "Done. Remove 'opentelemetry_postgres' from shared_preload_libraries and restart PostgreSQL."
  exit 0
fi

[ "$MAJOR" -ge 14 ] && [ "$MAJOR" -le 17 ] || die "PostgreSQL $MAJOR is not supported (14-17)"

# ---- get the files ---------------------------------------------------------
if [ -n "$FROM" ]; then
  SRC="$FROM"
  if [ -f "$SRC" ]; then
    TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
    tar -xzf "$SRC" -C "$TMP"; SRC="$(find "$TMP" -name opentelemetry_postgres.so -exec dirname {} \; | head -n1)"
  fi
else
  case "$(uname -s)" in Linux) ;; *) die "only Linux is supported";; esac
  case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) die "unsupported CPU architecture: $(uname -m)" ;;
  esac
  if ldd --version 2>&1 | grep -qi musl; then LIBC=musl; else LIBC=glibc; fi

  if [ -n "${OTEL_PG_BASE_URL:-}" ]; then   # mirror / air-gapped: a directory holding SHA256SUMS and the tarballs
    BASE="${OTEL_PG_BASE_URL%/}"
  elif [ "$VERSION" = "latest" ]; then
    BASE="https://github.com/$REPO/releases/latest/download"
  else
    BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
  fi
  fetch() { # url dest
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -qO "$2" "$1"
    else die "need curl or wget to download the release (or download the tarball yourself and use --from)"; fi
  }
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" || die "cannot download $BASE/SHA256SUMS (is the release published and the repo public?)"
  TARBALL="$(grep -E "pg${MAJOR}-linux-${ARCH}-${LIBC}\.tar\.gz\$" "$TMP/SHA256SUMS" | awk '{print $2}' | head -n1)"
  [ -n "$TARBALL" ] || die "no build for PostgreSQL $MAJOR / linux-$ARCH / $LIBC in this release"
  say "Downloading $TARBALL"
  fetch "$BASE/$TARBALL" "$TMP/$TARBALL"
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$TMP" && grep " $TARBALL\$" SHA256SUMS | sha256sum -c - >/dev/null) || die "checksum mismatch for $TARBALL"
  else
    say "warning: sha256sum not found; skipping checksum verification"
  fi
  tar -xzf "$TMP/$TARBALL" -C "$TMP"
  SRC="$(find "$TMP" -name opentelemetry_postgres.so -exec dirname {} \; | head -n1)"
fi

[ -f "$SRC/opentelemetry_postgres.so" ] || die "opentelemetry_postgres.so not found in $SRC"

say "Installing for PostgreSQL $MAJOR ($WHERE)"
$SUDO mkdir -p "$EXTDIR"
$SUDO install -m 755 "$SRC/opentelemetry_postgres.so" "$PKGLIBDIR/opentelemetry_postgres.so"
$SUDO install -m 644 "$SRC"/opentelemetry_postgres.control "$EXTDIR/"
$SUDO install -m 644 "$SRC"/opentelemetry_postgres--*.sql "$EXTDIR/"

cat <<EOF

Installed:
  $PKGLIBDIR/opentelemetry_postgres.so
  $EXTDIR/opentelemetry_postgres.{control,--*.sql}

Next steps:
  1. In postgresql.conf (append to any existing shared_preload_libraries):
       shared_preload_libraries = 'opentelemetry_postgres'
       otel.endpoint = 'http://localhost:4318/v1/traces'
  2. Restart PostgreSQL (a reload is not enough).
  3. Optional, for otel_stats():  CREATE EXTENSION opentelemetry_postgres;
EOF
