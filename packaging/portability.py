#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["opentelemetry-proto"]
# ///
"""Load the release tarballs into unrelated PostgreSQL installs and check a traced query
arrives, parented, at a collector. None of the containers has compilers or server headers.

    packaging/portability.py [DIST_DIR]            # needs docker and the tarballs from packaging/Dockerfile.*
    packaging/portability.py dist pg17-ubuntu22    # run selected scenarios
"""
import os
import secrets
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test"))
from integration import Sink, attrs  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

DEB_PGDG = (
    "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "
    "postgresql-common curl ca-certificates gnupg >/dev/null; "
    "/usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y >/dev/null 2>&1; "
    "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends postgresql-{v} >/dev/null"
)
DEB_DISTRO = (
    "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "
    "postgresql >/dev/null"
)

# name, image, pg major, setup command (run as root in the container), PGBIN, extra install.sh args
SCENARIOS = [
    # official Docker Hub images
    *[(f"pg{v}-official", f"postgres:{v}", v, "true", f"/usr/lib/postgresql/{v}/bin", "") for v in (14, 15, 16, 17)],
    ("pg17-official-bookworm", "postgres:17-bookworm", 17, "true", "/usr/lib/postgresql/17/bin", ""),
    # older glibc than the build host (build: Debian 12 / 2.36; Ubuntu 22.04 is 2.35), PGDG, server-only (no pg_config)
    ("pg17-ubuntu22-pgdg", "ubuntu:22.04", 17, DEB_PGDG.format(v=17), "/usr/lib/postgresql/17/bin", ""),
    ("pg14-ubuntu22-pgdg", "ubuntu:22.04", 14, DEB_PGDG.format(v=14), "/usr/lib/postgresql/14/bin", ""),
    # forced no-pg_config path (what server-only installs without the -dev package use)
    ("pg16-ubuntu24-no-pg_config", "ubuntu:24.04", 16, DEB_DISTRO, "/usr/lib/postgresql/16/bin", "--pg-bin /usr/lib/postgresql/16/bin"),
    ("pg17-official-no-pg_config", "postgres:17", 17, "true", "/usr/lib/postgresql/17/bin", "--pg-bin /usr/lib/postgresql/17/bin"),
    ("pg16-official-alpine-no-pg_config", "postgres:16-alpine", 16, "true", "/usr/local/bin", "--pg-bin /usr/local/bin"),
    # the distro's own PostgreSQL, not PGDG
    ("pg15-debian12-distro", "debian:bookworm", 15, DEB_DISTRO, "/usr/lib/postgresql/15/bin", ""),
    ("pg16-ubuntu24-distro", "ubuntu:24.04", 16, DEB_DISTRO, "/usr/lib/postgresql/16/bin", ""),
    # RHEL family: different directory layout (/usr/lib64/pgsql), AppStream module, server-only
    ("pg16-rocky9-appstream", "rockylinux:9", 16,
     "dnf -qy module enable postgresql:16 >/dev/null && dnf -qy install postgresql-server >/dev/null", "/usr/bin", ""),
    # PGDG RPMs: /usr/pgsql-N layout
    ("pg15-rocky9-pgdg", "rockylinux:9", 15,
     "dnf -qy install https://download.postgresql.org/pub/repos/yum/reporpms/EL-9-x86_64/pgdg-redhat-repo-latest.noarch.rpm >/dev/null; "
     "sed -i 's/$releasever_major\\.$releasever_minor/$releasever_major/' /etc/yum.repos.d/pgdg-redhat-all.repo; "
     "dnf -qy module disable postgresql >/dev/null; "
     "dnf -qy install postgresql15-server >/dev/null", "/usr/pgsql-15/bin", ""),
    # musl
    *[(f"pg{v}-official-alpine", f"postgres:{v}-alpine", v, "true", "/usr/local/bin", "") for v in (14, 15, 16, 17)],
    ("pg17-alpine-distro", "alpine:3.22", 17, "apk add --no-cache postgresql17 >/dev/null", "/usr/libexec/postgresql17", ""),
]


def run(name, image, v, setup, pgbin, extra, sink, dist):
    trace, parent = secrets.token_hex(16), secrets.token_hex(8)
    sink.clear()
    env = dict(PG_MAJOR=str(v), TARBALL_DIR="/dist", INSTALL_ARGS=extra, TRACE=trace, PARENT=parent,
               PGBIN=pgbin, SMOKE_NAME=name, ENDPOINT=f"http://host.docker.internal:{sink.port}")
    script = f"set -e; {setup}; /smoke.sh"
    cmd = ["docker", "run", "--rm", "--add-host=host.docker.internal:host-gateway", "-u", "root",
           "-v", f"{os.path.abspath(dist)}:/dist:ro", "-v", f"{HERE}/smoke.sh:/smoke.sh:ro"]
    for k, val in env.items():
        cmd += ["-e", f"{k}={val}"]
    r = subprocess.run(cmd + [image, "sh", "-c", script], capture_output=True, text=True)
    if r.returncode != 0:
        return False, f"container failed:\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
    spans = sink.settle(0.5)
    mine = [s for s in spans if s.trace_id.hex() == trace]
    if len(mine) != 1:
        return False, f"expected 1 span for the traced query, got {len(mine)} (total {len(spans)})\n{r.stdout[-800:]}"
    s = mine[0]
    a = attrs(s)
    if s.parent_span_id.hex() != parent or a.get("db.system.name") != "postgresql":
        return False, f"bad span: parent={s.parent_span_id.hex()} attrs={a}"
    if len(spans) != 1:
        return False, "the query without a traceparent was traced"
    pgline = [ln for ln in r.stdout.splitlines() if "PostgreSQL" in ln][:1]
    return True, (pgline[0] if pgline else "")


def main():
    dist = sys.argv[1] if len(sys.argv) > 1 else "dist"
    only = set(sys.argv[2:])
    sink = Sink(host="0.0.0.0")
    failed = 0
    for sc in SCENARIOS:
        if only and sc[0] not in only:
            continue
        t0 = time.time()
        ok, info = run(*sc, sink, dist)
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'}  {sc[0]:<26} {sc[1]:<22} {time.time() - t0:5.1f}s  {info}", flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
