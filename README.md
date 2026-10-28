# opentelemetry-postgres

OpenTelemetry traces for PostgreSQL queries, parented to the application's own spans through a
[sqlcommenter](https://google.github.io/sqlcommenter/) `traceparent` comment, with **no change to
your queries beyond that comment**. The time spent *inside* Postgres shows up in the same trace
as the request that caused it.

There are two ways to run it, producing the same spans (same names and semantic-convention attributes):

| | **Extension** (`opentelemetry_postgres`) | **Log converter** (`pg-log-tracer`) |
|---|---|---|
| Runs | inside Postgres, as a shared library | anywhere: a static binary / container reading Postgres's logs |
| Works on | self-managed Postgres 14–17 on Linux, incl. the stock `postgres` Docker image | **anything that produces Postgres logs: RDS, Aurora, Cloud SQL, Azure, …** |
| Server changes | `shared_preload_libraries` + restart | logging settings (`auto_explain`, statement duration, `log_line_prefix`) |
| Per-query cost | one memcpy; export is out of band | the cost of *your* statement logging |
| Fidelity | every traced query: exact times, buffers, WAL, row counts, failed queries | statements the server logs: ms-resolution times, plans/buffers via `auto_explain` |
| Latency | ~1 s | log pipeline latency (seconds; ≥ 1 min with the CloudWatch receiver's default) |

Use the extension where you control the server; use the log converter where you cannot load extensions.
Both are described below.

## Mode 1: the extension

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

## Install

On Linux, with PostgreSQL 14–17 already installed (any distribution: Debian/Ubuntu, RHEL/Rocky,
Alpine, the official Docker images, PGDG, or your own build). **No compiler or server headers needed.**

```bash
curl -fsSL https://raw.githubusercontent.com/practical-otel/opentelemetry-postgres/main/install.sh | sh
```

`install.sh` detects your PostgreSQL major version, CPU (x86_64 / aarch64) and libc (glibc / musl),
downloads the matching prebuilt library from the GitHub release, verifies its checksum, and copies
three files into the server's library and extension directories. It finds them with `pg_config`, or —
for server-only packages that don't ship it — by inspecting the install. Options:
`--pg-config PATH`, `--pg-bin DIR`, `--version X.Y.Z`, `--from DIR|TARBALL` (offline), `--uninstall`.
It never edits `postgresql.conf`.

Then enable it and **restart** PostgreSQL (it allocates shared memory and starts a worker):

```ini
shared_preload_libraries = 'opentelemetry_postgres'   # append if you already preload others
otel.endpoint = 'http://localhost:4318/v1/traces'      # a collector on the same host or pod
otel.service_name = 'orders-db'
```

Optionally `CREATE EXTENSION opentelemetry_postgres;` for `otel_stats()` (tracing does not need it).

The library is a normal PostgreSQL extension; it must match the server's major version, so there is
one build per major. It requires glibc ≥ 2.14 or musl and nothing else. Servers built with
`--enable-cassert` are not binary compatible. Managed services (RDS, Cloud SQL, …) don't allow custom
extensions; use a self-managed server.

### Build from source

```bash
make && sudo make install          # needs the server headers (postgresql-server-dev-N / postgresql-devel)
```

### Sideload into the stock `postgres` image (no custom image)

You don't need our image or any rebuild: mount the three files into the unmodified official image and
preload the library.

```bash
docker run -e POSTGRES_PASSWORD=pw \
  -v $PWD/ext/opentelemetry_postgres.so:/usr/lib/postgresql/17/lib/opentelemetry_postgres.so:ro \
  -v $PWD/ext/opentelemetry_postgres.control:/usr/share/postgresql/17/extension/opentelemetry_postgres.control:ro \
  -v $PWD/ext/opentelemetry_postgres--0.1.0.sql:/usr/share/postgresql/17/extension/opentelemetry_postgres--0.1.0.sql:ro \
  -e OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4318 \
  postgres:17 postgres -c shared_preload_libraries=opentelemetry_postgres
```

`ext/` is the unpacked release tarball for your major version (glibc for the Debian-based images, musl
for `-alpine`, whose directories are `/usr/local/lib/postgresql` and `/usr/local/share/postgresql/extension`).
[`examples/sideload`](examples/sideload) is a runnable compose version (`./run.sh`, verified on
`postgres:14` and `postgres:17`). The same files can be copied into your own image with `COPY`, or
into a volume by an init step.

### Docker

A convenience image (the official `postgres` image plus the extension) — not required; any Postgres install works with `install.sh`.

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

## What the extension records

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

## Extension limitations (v0.1)

* **Statements traced:** those run by the executor — `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `MERGE`. Utility statements (DDL, `COPY`, `CALL`, `VACUUM`) are not traced yet.
* **Errors:** failures during execution are traced; parse, analyse and plan-time errors (syntax errors, missing tables, constant-folded `1/0`) are not.
* **Prepared statements:** sqlcommenter travels in the SQL text, so a statement prepared once and executed repeatedly keeps the traceparent from the `Parse` that created it. Drivers that cache server-side prepared statements will therefore reuse a stale parent. SQL-level `EXECUTE` sees the `PREPARE` text, not the `EXECUTE` text.
* **Nested statements** (inside functions / triggers) are not linked as children of the outer span. They are only traced if they carry their own traceparent, or if `otel.trace_untraced = on`.
* **Duration** is executor start → end. For a cursor that is fetched slowly, that includes idle time between fetches.
* **Export:** OTLP/HTTP protobuf over plain `http://` to a nearby collector (no TLS, no gzip, no retry — a failed batch is dropped and counted). `getaddrinfo` is not time-bounded; prefer an IP or a local name.
* **Postgres versions:** 14, 15, 16 and 17 (the end-to-end suite passes on each in the official Docker images). 18 is not supported yet (the `ExecutorStart` hook changed).

## Mode 2: managed databases (RDS, Cloud SQL, Azure, …) with `pg-log-tracer`

Managed services won't load custom C extensions, but they do let you configure Postgres's own
logging, and a logged statement still contains the sqlcommenter comment. `pg-log-tracer`
(`logtrace/`, Go, one static binary, also a container) turns those log lines into the same parented spans.

```
 app ──SQL + /*traceparent*/──► Postgres ──logs──► log pipeline ──► pg-log-tracer ──OTLP traces──► Collector
                              (auto_explain,      (file, CloudWatch,    parse, merge plan +
                               statement duration) Cloud Logging, …)    duration, redact
```

**1. Configure Postgres logging** (in the parameter group / flags of your provider):

```ini
log_min_duration_statement = 0            # or a threshold, e.g. 100ms; spans exist only for logged statements
log_line_prefix = '%m [%p] %q%u@%d %e '   # must contain %p and %m (or %t); %e adds the SQLSTATE to error spans
log_timezone = 'UTC'                      # other zone abbreviations are ambiguous
# optional, for plans, row counts and buffers:
shared_preload_libraries = 'auto_explain'
auto_explain.log_min_duration = 0
auto_explain.log_analyze = on
auto_explain.log_buffers = on
auto_explain.log_timing = off
```

`log_min_duration_statement = 0` logs every statement, which can be a lot of log volume; use a threshold or
statement sampling (`log_min_duration_sample`, `log_statement_sample_rate`) as appropriate. Postgres's own log
contains full query text and literals regardless of what the tracer exports, so enabling statement logging is
a decision about your logs first.

**2. Run the converter**, with any of these inputs:

```bash
pg-log-tracer --file /var/log/postgresql/postgresql.log --log-line-prefix '%m [%p] %q%u@%d %e '   # follow a file
some-log-source | pg-log-tracer --log-line-prefix '...'                                              # stdin
pg-log-tracer --listen :4319 --log-line-prefix '...'   # receive OTLP logs (POST /v1/logs) from a Collector
```

Traces go to `OTEL_EXPORTER_OTLP_ENDPOINT` (or `--endpoint`). Other flags: `--service-name`,
`--query-text off|sanitized|full`, `--include-plans`, `--trace-untraced`, `--min-duration`. Binaries for
Linux/macOS/Windows (amd64/arm64) and a container image are built by the release workflow;
`cd logtrace && go build` works too. It logs counters every minute and on exit
(`lines=… records=… statements=… spans=…`) and warns if no line matches `--log-line-prefix`.

The OTLP-logs input is the integration point for cloud log sources: the Collector's receivers
(`filelog`, `awscloudwatch`, `googlecloudpubsub`, `azureeventhub`, …) read the provider's logs and an
`otlphttp` exporter forwards them to `--listen`. The demo `examples/demo-managed` does exactly this with
`filelog`; swap the receiver for your cloud:

```yaml
receivers:
  awscloudwatch:                # alpha; polls every logs.poll_interval (default 1m)
    region: eu-west-1
    logs:
      poll_interval: 10s
      groups:
        named:
          /aws/rds/instance/my-db/postgresql: {}
exporters:
  otlphttp/log-tracer:
    logs_endpoint: http://pg-log-tracer:4319/v1/logs
    encoding: proto
service:
  pipelines:
    logs: { receivers: [awscloudwatch], exporters: [otlphttp/log-tracer] }
```

**What you get compared with the extension.** Same span names and `db.*` attributes, plus
`postgresql.span.source = log`. Extra: utility statements (DDL, `COPY`, …) and parse/plan-time errors are traced,
because Postgres logs them. Less: no WAL counters or affected-row counts; row counts and buffers only come from an
`auto_explain` plan; timestamps have the log's millisecond precision and the end time is the log time (start = end − duration),
so they rely on the database and application clocks agreeing; failed statements have no duration (zero-length span).
The same sqlcommenter caveats apply (prepared statements keep the traceparent from the `Parse`; bound parameters
and `DETAIL`/`CONTEXT` lines are never exported). The collection name is inferred from the plan or, failing that,
a conservative reading of the SQL, and left out when unclear.

**Status.** Verified against real Postgres 17 logs: unit tests on captured log text, and `examples/demo-managed`
(stock `postgres` image with no extension → logs → Collector `filelog` → `pg-log-tracer` → traces parented to the
app's psycopg2 spans). **Not yet tested against actual RDS, Cloud SQL or Azure, or the cloud log receivers**
(CloudWatch event splitting of multi-line plans in particular); the parser accepts both one event per line
and one event per record.

## Development

```bash
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=... install
make PG_CONFIG=... integration    # throwaway cluster + fake collector; needs uv
docker build --target test --build-arg PG_MAJOR=16 .   # same suite, inside the official image
(cd logtrace && go test ./...)    # pg-log-tracer unit tests
examples/demo/run.sh              # extension  -> Collector, parentage verified
examples/demo-managed/run.sh      # stock postgres + logs -> pg-log-tracer -> Collector, parentage verified
```

Release artifacts are built by `packaging/Dockerfile.glibc` (on Debian 12, against PGDG headers, so the
library needs only glibc ≥ 2.14) and `packaging/Dockerfile.musl` (Alpine). `packaging/portability.py dist`
installs those tarballs with `install.sh` into stock PostgreSQL installs that have no build tools —
the official Docker images, PGDG and distro packages on Debian/Ubuntu, RHEL-family PGDG and AppStream
packages, Alpine — and checks a traced query reaches a collector with the right parent.

The integration test (`test/integration.py`) starts a private cluster, a fake OTLP
collector that decodes real protobuf with `opentelemetry-proto`, and asserts on the
resulting spans, including error spans, redaction, plans, collector outages and
ring overflow.

## Roadmap

* Extension: utility statements and `COPY`
* `pg-log-tracer`: native CloudWatch / Cloud Logging / Azure inputs without a Collector, JSON (`jsonlog`) input, tests against real managed services
* Parent/child spans for nested statements and per-plan-node spans from `analyze` plans
* Metrics (`db.client.operation.duration` server-side, ring/exporter health)
* Retry with backoff, TLS and gzip, or a Unix-socket/sidecar transport
* `tracestate` propagation; PostgreSQL 18

## License

Apache-2.0
