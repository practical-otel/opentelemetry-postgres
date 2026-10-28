#!/bin/sh
# Runs INSIDE a container that already has a PostgreSQL server installed (and nothing else
# from us). Installs the release tarball with install.sh, starts a throwaway cluster with the
# library preloaded, and runs traced queries. Expects:
#   PG_MAJOR  TARBALL_DIR  INSTALL_ARGS (optional)  ENDPOINT  TRACE  PARENT  PGBIN
set -eu
ARCH="$(uname -m)"
if ldd --version 2>&1 | grep -qi musl; then LIBC=musl; else LIBC=glibc; fi
TARBALL="$(ls "$TARBALL_DIR"/*-pg${PG_MAJOR}-linux-${ARCH}-${LIBC}.tar.gz)"

echo "--- install.sh ($TARBALL) ${INSTALL_ARGS:-}"
tar -xzf "$TARBALL" -C /tmp
# shellcheck disable=SC2086
/tmp/opentelemetry-postgres-*/install.sh --from "$TARBALL" ${INSTALL_ARGS:-}

DATA=/tmp/smoke-data
rm -rf "$DATA"; mkdir -p "$DATA"; chown postgres "$DATA"
as_pg() { su -s /bin/sh postgres -c "$1"; }

as_pg "$PGBIN/initdb -D $DATA -A trust -N >/dev/null"
cat >> "$DATA/postgresql.conf" <<CONF
listen_addresses = ''
unix_socket_directories = '/tmp'
port = 5499
shared_preload_libraries = 'opentelemetry_postgres'
otel.flush_interval = 200ms
CONF

echo "--- start"
as_pg "OTEL_EXPORTER_OTLP_ENDPOINT='$ENDPOINT' OTEL_SERVICE_NAME='smoke-$SMOKE_NAME' $PGBIN/pg_ctl -D $DATA -l /tmp/smoke.log -w start >/dev/null" \
  || { cat /tmp/smoke.log; exit 1; }
PSQL="$PGBIN/psql -h /tmp -p 5499 -U postgres -X -At"
as_pg "$PSQL -c 'create extension opentelemetry_postgres'"
as_pg "$PSQL -c \"select version()\"" | cut -c1-100
as_pg "$PSQL -c \"select 1 /*traceparent='00-$TRACE-$PARENT-01'*/\""
as_pg "$PSQL -c \"select 2\""    # no traceparent: must not be traced
sleep 2
echo "stats(enqueued|dropped|exported|failures|export_dropped|depth): $(as_pg "$PSQL -c 'select * from otel_stats()'")"
as_pg "$PGBIN/pg_ctl -D $DATA -m fast -w stop >/dev/null"
echo "--- ok"
