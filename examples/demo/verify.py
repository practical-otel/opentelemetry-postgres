#!/usr/bin/env python3
"""Send traffic to the demo app and prove the traces stitch together.

Each request carries a W3C traceparent, so the expected chain is

    caller (injected parent)
      └─ demo-app      SERVER  GET /orders/<id>          (Flask instrumentation)
           └─ demo-app CLIENT  SELECT                    (psycopg2 instrumentation)
                └─ demo-postgres SERVER  SELECT orders   (opentelemetry-postgres)

All spans are read back from the Collector's file exporter, i.e. after they have
travelled app -> collector and postgres -> collector over OTLP/HTTP.
Standard library only.
"""
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request

APP = os.environ.get("APP_URL", "http://localhost:8080")
FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "traces.jsonl")
SERVER, CLIENT = 2, 3


def call(method, path, trace, parent, body=None):
    req = urllib.request.Request(
        APP + path, method=method, data=json.dumps(body).encode() if body else None,
        headers={"traceparent": f"00-{trace}-{parent}-01", "content-type": "application/json"},
    )
    try:
        return urllib.request.urlopen(req, timeout=10).status
    except urllib.error.HTTPError as e:
        return e.code


def load():
    spans = []
    try:
        lines = open(FILE).read().splitlines()
    except FileNotFoundError:
        return spans
    for line in lines:
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue  # the exporter is still writing this line
        for rs in doc.get("resourceSpans", []):
            res = {a["key"]: next(iter(a["value"].values())) for a in rs["resource"].get("attributes", [])}
            for ss in rs["scopeSpans"]:
                for s in ss["spans"]:
                    s["service"] = res.get("service.name")
                    s["attrs"] = {a["key"]: next(iter(a["value"].values())) for a in s.get("attributes", [])}
                    spans.append(s)
    return spans


def wait_for(trace, want_services=("demo-app", "demo-postgres"), timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        spans = [s for s in load() if s["traceId"] == trace]
        # app: server + client; postgres: at least one
        if (any(s["service"] == "demo-postgres" for s in spans)
                and any(s["service"] == "demo-app" and s["kind"] == CLIENT for s in spans)
                and any(s["service"] == "demo-app" and s["kind"] == SERVER for s in spans)):
            return spans
        time.sleep(0.5)
    raise AssertionError(f"timed out waiting for spans of trace {trace}; have {[(s['service'], s['name']) for s in spans]}")


def tree(spans, root_parent):
    kids = {}
    for s in spans:
        kids.setdefault(s.get("parentSpanId", ""), []).append(s)
    out = [f"caller           parent={root_parent}"]

    def walk(pid, depth):
        for s in sorted(kids.get(pid, []), key=lambda x: int(x["startTimeUnixNano"])):
            kind = {SERVER: "SERVER", CLIENT: "CLIENT"}.get(s["kind"], "INTERNAL")
            out.append(f"{'  ' * depth}└─ {s['service']:<13} {kind:<6} {s['name']}  [{s['spanId'][:8]}]")
            walk(s["spanId"], depth + 1)
    walk(root_parent, 1)
    return "\n".join(out)


def check_chain(spans, parent):
    """Returns (flask_server, db_client, pg_span) after asserting the parentage."""
    flask = [s for s in spans if s["service"] == "demo-app" and s["kind"] == SERVER]
    assert len(flask) == 1 and flask[0]["parentSpanId"] == parent, "app SERVER span must be a child of the caller"
    clients = [s for s in spans if s["service"] == "demo-app" and s["kind"] == CLIENT]
    assert len(clients) == 1 and clients[0]["parentSpanId"] == flask[0]["spanId"], "psycopg2 CLIENT span must be a child of the app SERVER span"
    pg = [s for s in spans if s["service"] == "demo-postgres"]
    assert len(pg) == 1, f"expected exactly one postgres span, got {len(pg)}"
    assert pg[0]["parentSpanId"] == clients[0]["spanId"], "postgres span must be a child of the psycopg2 CLIENT span"
    assert pg[0]["kind"] == SERVER
    return flask[0], clients[0], pg[0]


def main():
    for _ in range(60):  # app readiness
        try:
            urllib.request.urlopen(APP + "/healthz", timeout=2)
            break
        except Exception:
            time.sleep(1)

    results = []

    # 1. SELECT
    t, p = secrets.token_hex(16), secrets.token_hex(8)
    assert call("GET", "/orders/1", t, p) == 200
    spans = wait_for(t)
    _, _, pg = check_chain(spans, p)
    a = pg["attrs"]
    assert a["db.system.name"] == "postgresql" and a["db.namespace"] == "shop"
    assert a["db.operation.name"] == "SELECT" and a["db.collection.name"] == "orders"
    assert a["db.query.text"] == "SELECT id, customer, total FROM orders WHERE id = ?", a["db.query.text"]
    assert int(a["db.response.returned_rows"]) == 1
    assert "Scan using orders_pkey on orders" in a["postgresql.query.plan"], a.get("postgresql.query.plan")
    results.append(("GET /orders/1", t, p, spans))

    # 2. INSERT (customer email must never reach the telemetry)
    t, p = secrets.token_hex(16), secrets.token_hex(8)
    assert call("POST", "/orders", t, p, {"customer": "carol@example.com", "total": 99}) == 201
    spans = wait_for(t)
    _, _, pg = check_chain(spans, p)
    a = pg["attrs"]
    assert a["db.operation.name"] == "INSERT" and a["db.collection.name"] == "orders"
    assert int(a["postgresql.rows.affected"]) == 1
    assert "carol@example.com" not in json.dumps(pg), "literal leaked into the postgres span"
    assert int(a["postgresql.wal.records"]) > 0
    results.append(("POST /orders", t, p, spans))

    # 3. runtime SQL error
    t, p = secrets.token_hex(16), secrets.token_hex(8)
    assert call("GET", "/fail", t, p) == 500
    spans = wait_for(t)
    flask, client, pg = check_chain(spans, p)
    assert pg["attrs"]["error.type"] == "22012" and pg["status"]["code"] == 2
    results.append(("GET /fail", t, p, spans))

    print()
    for name, t, p, spans in results:
        print(f"{name}   trace={t}")
        print(tree(spans, p))
        print()
    print("PASS: every postgres span is parented to the application's psycopg2 CLIENT span, in the application's trace")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print(f"FAIL: {e}")
        sys.exit(1)
