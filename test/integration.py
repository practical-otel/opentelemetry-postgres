#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["opentelemetry-proto"]
# ///
"""End-to-end test.

Starts a throwaway PostgreSQL cluster with the extension preloaded and a fake
OTLP/HTTP collector, runs queries that carry sqlcommenter traceparents, and
checks the spans the collector receives.

    make install && uv run test/integration.py        # PG_CONFIG=... to pick a server

Needs `opentelemetry-proto` (uv installs it from the header above).
"""
import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

from opentelemetry.proto.collector.trace.v1 import trace_service_pb2

PG_CONFIG = os.environ.get("PG_CONFIG", "pg_config")
BINDIR = subprocess.check_output([PG_CONFIG, "--bindir"], text=True).strip()

TRACE = "4bf92f3577b34da6a3ce929d0e0e4736"
PARENT = "00f067aa0ba902b7"


def tp(flags="01", trace=TRACE, parent=PARENT):
    return f"/*traceparent='00-{trace}-{parent}-{flags}'*/"


# --------------------------------------------------------------------------
# Fake collector
# --------------------------------------------------------------------------
class Sink:
    def __init__(self, host="127.0.0.1"):
        self.spans = []
        self.resources = []
        self.lock = threading.Lock()
        self.fail = False
        self.delay = 0
        sink = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                time.sleep(sink.delay)
                if sink.fail:
                    self.send_response(503)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                req = trace_service_pb2.ExportTraceServiceRequest()
                req.ParseFromString(body)
                with sink.lock:
                    for rs in req.resource_spans:
                        sink.resources.append(
                            {kv.key: kv.value.string_value for kv in rs.resource.attributes}
                        )
                        for ss in rs.scope_spans:
                            assert ss.scope.name == "opentelemetry-postgres"
                            for s in ss.spans:
                                sink.spans.append(s)
                assert self.path == "/v1/traces", self.path
                assert self.headers["Content-Type"] == "application/x-protobuf"
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

        self.srv = http.server.ThreadingHTTPServer((host, 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def clear(self):
        with self.lock:
            self.spans.clear()

    def wait(self, n=1, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if len(self.spans) >= n:
                    return list(self.spans)
            time.sleep(0.05)
        raise AssertionError(f"expected {n} span(s), got {len(self.spans)}")

    def settle(self, seconds=1.0):
        """Wait long enough for a flush, then return whatever arrived."""
        time.sleep(seconds)
        with self.lock:
            return list(self.spans)


def attrs(span):
    out = {}
    for kv in span.attributes:
        v = kv.value
        out[kv.key] = getattr(v, v.WhichOneof("value"))
    return out


# --------------------------------------------------------------------------
# Throwaway cluster
# --------------------------------------------------------------------------
class Cluster:
    def __init__(self, sink, extra=""):
        self.dir = tempfile.mkdtemp(prefix="otelpg-")
        self.sink = sink
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        data = os.path.join(self.dir, "data")
        self.run("initdb", "-D", data, "-U", "postgres", "--auth=trust", "-N")
        with open(os.path.join(data, "postgresql.conf"), "a") as f:
            f.write(
                f"""
port = {self.port}
listen_addresses = ''
unix_socket_directories = '{self.dir}'
shared_preload_libraries = 'opentelemetry_postgres'
otel.endpoint = 'http://127.0.0.1:{sink.port}/v1/traces'
otel.flush_interval = 100ms
otel.service_name = 'test-postgres'
fsync = off
{extra}
"""
            )
        self.data = data
        self.run("pg_ctl", "-D", data, "-l", os.path.join(self.dir, "log"), "-w", "start")

    def run(self, exe, *args, **kw):
        return subprocess.run(
            [os.path.join(BINDIR, exe), *args], check=True, capture_output=True, text=True, **kw
        )

    def psql(self, sql, db="postgres", check=True):
        r = subprocess.run(
            [os.path.join(BINDIR, "psql"), "-h", self.dir, "-p", str(self.port), "-U", "postgres",
             "-d", db, "-X", "-q", "-At", "-v", "ON_ERROR_STOP=1", "-c", sql],
            capture_output=True, text=True,
        )
        if check and r.returncode != 0:
            raise AssertionError(f"psql failed: {r.stderr}")
        return r

    def stats(self):
        cols = "enqueued dropped exported export_failures export_dropped queue_depth".split()
        vals = self.psql("select * from otel_stats()").stdout.strip().split("|")
        return dict(zip(cols, map(int, vals)))

    def stop(self):
        subprocess.run([os.path.join(BINDIR, "pg_ctl"), "-D", self.data, "-m", "fast", "stop"],
                       capture_output=True)

    def log(self):
        return open(os.path.join(self.dir, "log")).read()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
TESTS = []


def test(f):
    TESTS.append(f)
    return f


@test
def suffix_comment_becomes_parent(c, sink):
    c.psql("select count(*) from orders where id = 42 " + tp())
    [s] = sink.wait(1)
    a = attrs(s)
    assert s.trace_id.hex() == TRACE
    assert s.parent_span_id.hex() == PARENT
    assert len(s.span_id) == 8 and s.span_id != bytes(8)
    assert s.kind == 2  # SERVER
    assert s.name == "SELECT orders", s.name
    assert a["db.system.name"] == "postgresql"
    assert a["db.namespace"] == "postgres"
    assert a["db.operation.name"] == "SELECT"
    assert a["db.collection.name"] == "orders"
    assert a["db.query.summary"] == "SELECT orders"
    assert a["db.response.returned_rows"] == 1
    # literal redacted, sqlcommenter payload stripped
    assert a["db.query.text"] == "select count(*) from orders where id = ?", a["db.query.text"]
    assert s.end_time_unix_nano > s.start_time_unix_nano
    assert abs(s.start_time_unix_nano / 1e9 - time.time()) < 60
    assert sink.resources[-1]["service.name"] == "test-postgres"


@test
def prefix_comment_works(c, sink):
    c.psql(tp(trace="0af7651916cd43dd8448eb211c80319c") + " select 1")
    [s] = sink.wait(1)
    assert s.trace_id.hex() == "0af7651916cd43dd8448eb211c80319c"
    assert attrs(s)["db.query.text"] == "select ?"


@test
def not_sampled_parent_is_honoured(c, sink):
    c.psql("select 1 " + tp(flags="00"))
    assert sink.settle() == []


@test
def no_traceparent_no_span_by_default(c, sink):
    c.psql("select 1")
    c.psql("select 2 /*application='x'*/")
    assert sink.settle() == []


@test
def traceparent_outside_a_comment_is_ignored(c, sink):
    c.psql(f"select $$traceparent='00-{TRACE}-{PARENT}-01'$$")
    c.psql(f"select 'traceparent=''00-{TRACE}-{PARENT}-01'''")
    assert sink.settle() == []


@test
def malformed_traceparent_is_ignored(c, sink):
    for bad in (
        f"00-{TRACE.upper()}-{PARENT}-01",       # uppercase
        f"00-{'0' * 32}-{PARENT}-01",            # zero trace id
        f"00-{TRACE}-{'0' * 16}-01",             # zero span id
        f"ff-{TRACE}-{PARENT}-01",               # forbidden version
        f"00-{TRACE[:-1]}-{PARENT}-01",          # short
    ):
        c.psql(f"select 1 /*traceparent='{bad}'*/")
    assert sink.settle() == []


@test
def dml_names_target_and_counts_rows(c, sink):
    c.psql("insert into orders select g, 'x' from generate_series(100, 104) g " + tp())
    c.psql("update orders set note = 'secret' where id >= 100 " + tp())
    c.psql("delete from orders where id >= 100 " + tp())
    spans = sink.wait(3)
    by_op = {attrs(s)["db.operation.name"]: s for s in spans}
    for op in ("INSERT", "UPDATE", "DELETE"):
        a = attrs(by_op[op])
        assert a["db.collection.name"] == "orders", (op, a)
        assert a["postgresql.rows.affected"] == 5, (op, a)
        assert by_op[op].name == f"{op} orders"
        assert "db.response.returned_rows" not in a
    assert attrs(by_op["INSERT"])["postgresql.wal.records"] > 0
    assert "secret" not in attrs(by_op["UPDATE"])["db.query.text"]


@test
def join_has_no_collection(c, sink):
    c.psql("select * from orders o join orders p on p.id = o.id " + tp())  # same table -> one
    c.psql("select * from orders o, pg_class p where false " + tp())       # two tables
    one, two = sink.wait(2)
    assert attrs(one)["db.collection.name"] == "orders"
    assert "db.collection.name" not in attrs(two)
    assert two.name == "SELECT postgres"


@test
def failing_query_emits_error_span(c, sink):
    r = c.psql("select 1/(id-id) from orders " + tp(), check=False)
    assert r.returncode != 0
    [s] = sink.wait(1)
    a = attrs(s)
    assert a["error.type"] == "22012"
    assert a["db.response.status_code"] == "22012"
    assert s.status.code == 2  # ERROR
    assert s.status.message == ""  # default query_text=sanitized: no message text
    # and the connection is still usable afterwards
    assert c.psql("select 7").stdout.strip() == "7"


@test
def error_message_only_with_full_text(c, sink):
    c.psql("set otel.query_text = 'full'; select 1/(id-id) from orders " + tp(), check=False)
    [s] = sink.wait(1)
    assert s.status.message == "division by zero", s.status.message
    assert "1/(id-id)" in attrs(s)["db.query.text"]


@test
def query_text_off(c, sink):
    c.psql("set otel.query_text = 'off'; select 1 " + tp())
    [s] = sink.wait(1)
    assert "db.query.text" not in attrs(s)


@test
def plans_estimate_and_analyze(c, sink):
    c.psql("set otel.plan_mode='estimate'; set otel.plan_min_duration=0; "
           "select * from orders where note = 'hunter2@example.com' " + tp())
    c.psql("set otel.plan_mode='analyze'; set otel.plan_min_duration=0; "
           "select * from orders where note = 'hunter2@example.com' " + tp())
    est, ana = sink.wait(2)
    pe, pa = attrs(est), attrs(ana)
    assert "Seq Scan on orders" in pe["postgresql.query.plan"], pe
    assert "actual" not in pe["postgresql.query.plan"] and "rows=" in pe["postgresql.query.plan"]
    assert pe["postgresql.query.plan.analyzed"] == 0
    assert "(actual rows=" in pa["postgresql.query.plan"], pa["postgresql.query.plan"]
    assert pa["postgresql.query.plan.analyzed"] == 1
    for p in (pe, pa):
        assert "hunter2" not in p["postgresql.query.plan"], "string literal leaked into plan"
        assert "hunter2" not in p["db.query.text"]


@test
def plan_threshold_and_default_off(c, sink):
    c.psql("select * from orders " + tp())                                   # default: off
    c.psql("set otel.plan_mode='estimate'; select * from orders " + tp())    # < 100ms
    spans = sink.wait(2)
    assert all("postgresql.query.plan" not in attrs(s) for s in spans)


@test
def min_duration_filters_fast_spans_but_not_errors(c, sink):
    c.psql("set otel.min_duration = '10s'; select 1 " + tp())
    c.psql("set otel.min_duration = '10s'; select 1/(id-id) from orders " + tp(), check=False)
    [s] = sink.wait(1)
    assert attrs(s)["error.type"] == "22012"
    assert len(sink.settle()) == 1


@test
def untraced_root_spans_when_enabled(c, sink):
    c.psql("set otel.trace_untraced = on; select 1; select 2")
    spans = sink.wait(2)
    assert all(s.parent_span_id == b"" for s in spans)
    assert len({s.trace_id for s in spans}) == 2 and all(len(s.trace_id) == 16 for s in spans)


@test
def buffer_counters(c, sink):
    c.psql("select count(*) from orders " + tp())
    [s] = sink.wait(1)
    a = attrs(s)
    assert a.get("postgresql.buffers.shared.hit", 0) + a.get("postgresql.buffers.shared.read", 0) > 0


@test
def disabled_means_no_spans(c, sink):
    c.psql("set otel.enabled = off; select 1 " + tp())
    assert sink.settle() == []


@test
def cursors_and_functions(c, sink):
    c.psql("begin; declare cur cursor for select * from orders " + tp() +
           "; fetch 1 from cur; fetch 1 from cur; commit")
    spans = sink.wait(1)
    assert len(spans) == 1 and spans[0].parent_span_id.hex() == PARENT
    sink.clear()
    # a plpgsql body: the nested statement has no traceparent of its own
    c.psql("create or replace function f() returns int language plpgsql as "
           "$$ begin return (select count(*) from orders); end $$")
    c.psql("select f() " + tp())
    spans = sink.wait(1)
    assert len(sink.settle()) == 1


@test
def collector_outage_never_hurts_queries(c, sink):
    before = c.stats()
    sink.fail = True
    for _ in range(5):
        assert c.psql("select 1 " + tp()).stdout.strip() == "1"
    deadline = time.time() + 10
    while time.time() < deadline and c.stats()["export_failures"] == before["export_failures"]:
        time.sleep(0.1)
    st = c.stats()
    assert st["export_failures"] > before["export_failures"]
    assert st["export_dropped"] >= before["export_dropped"] + 1
    sink.fail = False
    sink.clear()
    c.psql("select 2 " + tp())
    sink.wait(1)  # exporter recovers on its own


@test
def stats_view(c, sink):
    c.psql("select 1 " + tp())
    sink.wait(1)
    st = c.stats()
    assert st["enqueued"] >= 1 and st["exported"] >= 1 and st["queue_depth"] == 0


@test
def sanitizer_cases(c, sink):
    cases = [
        ("select 'it''s' as a1, $$x$$ as c, $t$ y $t$ as d, 1.5e-3 as e, .5 as f, 7 as \"col 1\", E'a\\'b' as g from pg_class limit 3",
         "select ? as a1, ? as c, ? as d, ? as e, ? as f, ? as \"col 1\", ? as g from pg_class limit ?"),
        ("select  1 /* a /* nested */ b */ , -- tail\n 2\n  from pg_class", "select ? , ? from pg_class"),
        ("select relname from pg_class where relname = 'x'  and relnatts > 2 ", "select relname from pg_class where relname = ? and relnatts > ?"),
    ]
    for sql, _ in cases:
        c.psql(sql + " " + tp())
    spans = sink.wait(len(cases))
    got = sorted(attrs(s)["db.query.text"] for s in spans)
    want = sorted(w for _, w in cases)
    assert got == want, f"\n got {got}\nwant {want}"


def overflow_scenario():
    """A stalled collector must not slow or fail queries; excess spans are dropped and counted."""
    sink = Sink()
    sink.delay = 4
    c = Cluster(sink, extra="otel.buffer_slots = 64\notel.export_timeout = 10s\notel.max_batch = 8")
    try:
        c.psql("create extension opentelemetry_postgres")
        t0 = time.time()
        c.psql(";".join(f"select {i} {tp()}" for i in range(300)))
        for i in range(10):
            assert c.psql(f"select {i} {tp()}").returncode == 0
        elapsed = time.time() - t0
        st = c.stats()
        assert st["dropped"] > 0, st
        assert st["enqueued"] + st["dropped"] == 310, st
        assert elapsed < 3, f"queries were slowed by the stalled exporter ({elapsed:.1f}s)"
        sink.delay = 0
        deadline = time.time() + 20
        while time.time() < deadline and c.stats()["queue_depth"] > 0:
            time.sleep(0.2)
        st = c.stats()
        assert st["queue_depth"] == 0 and st["exported"] == st["enqueued"], st
        print("ok    ring_overflow_drops_instead_of_blocking", st)
        return 0
    except Exception as e:  # noqa: BLE001
        print(f"FAIL  ring_overflow_drops_instead_of_blocking: {type(e).__name__}: {e}")
        return 1
    finally:
        c.cleanup()


def main():
    sink = Sink()
    c = Cluster(sink)
    failed = 0
    try:
        c.psql("create extension opentelemetry_postgres")
        c.psql("create table orders(id int primary key, note text); "
               "insert into orders values (1, 'a'), (2, 'b')")
        for t in TESTS:
            sink.clear()
            try:
                t(c, sink)
                print(f"ok    {t.__name__}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL  {t.__name__}: {type(e).__name__}: {e}")
        failed += overflow_scenario()
        log = c.log()
        if "TRAP" in log or "terminated by signal" in log:
            failed += 1
            print("FAIL  server log contains a crash")
    finally:
        keep = os.environ.get("KEEP")
        if keep:
            print("cluster kept at", c.dir)
        else:
            c.cleanup()
    print(f"\n{len(TESTS) + 1 - failed}/{len(TESTS) + 1} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
