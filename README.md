# opentelemetry-postgres

A PostgreSQL extension that turns queries into OpenTelemetry spans — with **no
changes to your queries beyond a [sqlcommenter](https://google.github.io/sqlcommenter/)
comment** — and ships them to an OTLP collector **out of band**.

```
┌──────────── client ────────────┐         ┌───────────────── PostgreSQL ─────────────────┐
│ span: SELECT orders  (CLIENT)  │  SQL +  │ backend: executor hook                       │
│   SELECT … /*traceparent='…'*/ ├────────►│   parse traceparent, 2 clock reads, 1 memcpy │
└────────────────────────────────┘ comment │        │  lock-free ring in shared memory    │
                                           │        ▼                                     │
                                           │ bgworker: ids, sanitise, protobuf, HTTP ─────┼──► OTLP collector
                                           └──────────────────────────────────────────────┘
```

The database span (`kind=SERVER`) becomes a child of the client's span, so the
time spent *inside* Postgres shows up in the same trace as the application.

## Why it is cheap

| On the query path (backend) | Off the query path (background worker) |
|---|---|
| bounded scan (first/last 1 KiB) of the query text for `traceparent='…'` | span id / trace id generation |
| two `GetCurrentTimestamp()` calls | query text sanitising (literal redaction) |
| copy of buffer/WAL counters (already maintained by Postgres) | protobuf encoding |
| one `memcpy` into a pre-allocated ring slot | HTTP, retries of the connection, timeouts |
| `EXPLAIN` text, **only** if you enable plans and the query is slow | |

* Queries without a sampled `traceparent` are not traced (default) and pay for one bounded text scan.
* The ring is a bounded lock-free MPSC queue. It **never blocks a backend**: when full, spans are dropped and counted (`otel_stats()`).
* A dead or slow collector costs nothing but dropped spans; the worker bounds every socket operation with `otel.export_timeout`.
* The worker never connects to a database, so it holds no snapshots or locks.

Measured on a 4-core orb with PostgreSQL 15 and `pgbench` point selects: single
client — no measurable difference between disabled and every-query-traced
(±5 % run-to-run noise); 16 clients at ~100k queries/s with the exporter and a
Python collector competing for the same cores — about 14 % lower throughput
when *every* query is traced. Treat that as an upper bound and measure on your workload.

## Quick start

```bash
make && sudo make install                  # needs the server headers (postgresql-server-dev-N)
```

`postgresql.conf` (restart required — it allocates shared memory and starts a worker):

```ini
shared_preload_libraries = 'opentelemetry_postgres'
otel.endpoint = 'http://localhost:4318/v1/traces'   # a local OpenTelemetry Collector
otel.service_name = 'orders-db'
```

Optionally `CREATE EXTENSION opentelemetry_postgres;` to get `otel_stats()`.

Then have your application add a sqlcommenter `traceparent` comment, e.g.

```sql
SELECT * FROM orders WHERE id = $1 /*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01'*/
```

Most OpenTelemetry SQL instrumentations can do this for you
(e.g. `enable_commenter=True` in the Python psycopg/SQLAlchemy instrumentations,
`sqlcommenter` options in Java/Go/Node instrumentations).

### Docker

```bash
docker build -t opentelemetry-postgres --build-arg PG_MAJOR=17 .   # 14, 15, 16, 17
docker run -e POSTGRES_PASSWORD=pw -e OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4318 opentelemetry-postgres
```

The image is the official `postgres` image plus the extension, with
`shared_preload_libraries` already set. `OTEL_EXPORTER_OTLP_ENDPOINT` and
`OTEL_SERVICE_NAME` are honoured.

### Try it end to end

[`examples/demo`](examples/demo) runs a Flask app (OpenTelemetry Flask + psycopg2
instrumentation with `enable_commenter`), PostgreSQL with this extension, and a real
OpenTelemetry Collector. `examples/demo/run.sh` sends requests carrying a `traceparent`
and checks that every Postgres span is a child of the app's database client span:

```
GET /orders/1   trace=bc5054be1feec40310b3e51943ab0461
caller           parent=247bbe199b5ef730
  └─ demo-app      SERVER GET /orders/<int:order_id>
    └─ demo-app      CLIENT SELECT
      └─ demo-postgres SERVER SELECT orders
```

## What you get

Attributes follow the [database client span semantic conventions](https://opentelemetry.io/docs/specs/semconv/database/);
Postgres-specific values that semconv does not define use the `postgresql.` namespace.

| Attribute | Notes |
|---|---|
| `db.system.name` | `postgresql` |
| `db.namespace` | database name |
| `db.operation.name` | `SELECT` `INSERT` `UPDATE` `DELETE` `MERGE` |
| `db.collection.name` | when the statement touches one table (target table for DML) |
| `db.query.summary` | e.g. `SELECT orders` |
| `db.query.text` | sanitised by default; see `otel.query_text` |
| `db.response.returned_rows` | `SELECT` only |
| `db.response.status_code`, `error.type`, span status | SQLSTATE, on error |
| `postgresql.rows.affected` | DML |
| `postgresql.query.id` | when `compute_query_id` is on |
| `postgresql.buffers.shared.{hit,read,dirtied,written}`, `postgresql.buffers.temp.{read,written}` | deltas for the statement, non-zero only |
| `postgresql.wal.{records,bytes}` | deltas for the statement, non-zero only |
| `postgresql.query.plan`, `.plan.format`, `.plan.analyzed` | opt-in, see below |
| `postgresql.backend.pid` | |

Span name is `{operation} {collection}` (falling back to the database name).
Span kind is `SERVER`: it is the database side of the client's `CLIENT` span.
Resource attributes: `service.name`, `service.version` (server version).

### Query plans

```ini
otel.plan_mode = 'estimate'     # planner output only
# otel.plan_mode = 'analyze'    # + actual row counts per node (no per-node timing)
otel.plan_min_duration = 100ms  # only attach plans to slow queries (-1 disables)
```

Plans are built in the backend (the executor state is needed) but only for spans
that pass the duration gate, in the same way `auto_explain` does. `analyze` mode
adds per-node row counting to *every* traced query, so it costs more.
String literals in plans are redacted unless `otel.query_text = full`.

### Privacy

`otel.query_text = sanitized` (default) strips comments (including the sqlcommenter
payload) and replaces string, dollar-quoted and numeric literals with `?`.
`off` omits the text; `full` sends it verbatim, and also adds the error message to
failed spans. Numeric constants inside plans are *not* redacted — which is why
plans are off by default.

## Configuration

| GUC | Default | Context | |
|---|---|---|---|
| `otel.enabled` | `on` | superuser | master switch |
| `otel.trace_untraced` | `off` | superuser | also create root spans for queries without a traceparent |
| `otel.min_duration` | `0` | superuser | drop faster spans (failures are always kept) |
| `otel.query_text` | `sanitized` | superuser | `off` / `sanitized` / `full` |
| `otel.plan_mode` | `off` | superuser | `off` / `estimate` / `analyze` |
| `otel.plan_min_duration` | `100ms` | superuser | |
| `otel.buffer_slots` | `2048` | restart | ring size (rounded up to a power of 2) |
| `otel.slot_size` | `4096` | restart | bytes per span incl. query text and plan; longer text is truncated |
| `otel.flush_interval` | `1s` | reload | max time a span waits before export |
| `otel.max_batch` | `512` | reload | spans per OTLP request |
| `otel.export_timeout` | `5s` | reload | connect/send/receive timeout |
| `otel.endpoint` | env / `http://localhost:4318/v1/traces` | reload | falls back to `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`, then `OTEL_EXPORTER_OTLP_ENDPOINT` + `/v1/traces` |
| `otel.service_name` | env / `postgresql` | reload | falls back to `OTEL_SERVICE_NAME` |

Shared memory used: `buffer_slots × slot_size` (8 MiB by default).

`SELECT * FROM otel_stats();` returns `enqueued`, `dropped`, `exported`,
`export_failures`, `export_dropped`, `queue_depth`. Alert on `dropped` and `export_failures`.

## Limitations (v0.1)

* **Statements traced:** those run by the executor — `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `MERGE`. Utility statements (DDL, `COPY`, `CALL`, `VACUUM`) are not traced yet.
* **Errors:** failures during execution are traced; parse, analyse and plan-time errors (syntax errors, missing tables, constant-folded `1/0`) are not.
* **Prepared statements:** sqlcommenter travels in the SQL text, so a statement prepared once and executed repeatedly keeps the traceparent from the `Parse` that created it. Drivers that cache server-side prepared statements will therefore reuse a stale parent. SQL-level `EXECUTE` sees the `PREPARE` text, not the `EXECUTE` text.
* **Nested statements** (inside functions / triggers) are not linked as children of the outer span. They are only traced if they carry their own traceparent, or if `otel.trace_untraced = on`.
* **Duration** is executor start → end. For a cursor that is fetched slowly, that includes idle time between fetches.
* **Export:** OTLP/HTTP protobuf over plain `http://` to a nearby collector (no TLS, no gzip, no retry — a failed batch is dropped and counted). `getaddrinfo` is not time-bounded; prefer an IP or a local name.
* **Postgres versions:** 14, 15, 16 and 17 (the end-to-end suite passes on each in the official Docker images). 18 is not supported yet (the `ExecutorStart` hook changed).

## Development

```bash
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=... install
make PG_CONFIG=... integration    # throwaway cluster + fake collector; needs uv
docker build --target test --build-arg PG_MAJOR=16 .   # same suite, inside the official image
```

The integration test (`test/integration.py`) starts a private cluster, a fake OTLP
collector that decodes real protobuf with `opentelemetry-proto`, and asserts on the
resulting spans, including error spans, redaction, plans, collector outages and
ring overflow.

## Roadmap

* Utility statements and `COPY`
* Parent/child spans for nested statements and per-plan-node spans from `analyze` plans
* Metrics (`db.client.operation.duration` server-side, ring/exporter health)
* Retry with backoff, TLS and gzip, or a Unix-socket/sidecar transport
* `tracestate` propagation; PostgreSQL 18

## License

Apache-2.0
