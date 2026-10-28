/*-------------------------------------------------------------------------
 * otel_otlp.c
 *	  Hand-rolled OTLP/protobuf encoding of ExportTraceServiceRequest.
 *
 * Only the handful of messages needed for spans are encoded, so there is no
 * protobuf library dependency.  Field numbers follow
 * opentelemetry/proto/{trace,common,resource}/v1.
 *
 * Runs in the background worker only.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/timestamp.h"

#include "otel.h"

/* Wire types */
#define PB_VARINT	0
#define PB_FIXED64	1
#define PB_LEN		2

/* AnyValue */
#define ANY_STRING	1
#define ANY_BOOL	2
#define ANY_INT		3

/* Span.SpanKind / Status.StatusCode */
#define SPAN_KIND_SERVER	2
#define OTLP_STATUS_ERROR		2

/* TimestampTz (µs since 2000-01-01) -> Unix epoch ns */
#define PG_TO_UNIX_US	INT64CONST(946684800000000)

static void
pb_varint(StringInfo b, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(b, (char) (v | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(b, (char) v);
}

static void
pb_tag(StringInfo b, int field, int wt)
{
	pb_varint(b, (uint64) (field << 3 | wt));
}

static void
pb_uint(StringInfo b, int field, uint64 v)
{
	pb_tag(b, field, PB_VARINT);
	pb_varint(b, v);
}

static void
pb_fixed64(StringInfo b, int field, uint64 v)
{
	int			i;

	pb_tag(b, field, PB_FIXED64);
	for (i = 0; i < 8; i++)
		appendStringInfoChar(b, (char) (v >> (8 * i)));
}

static void
pb_bytes(StringInfo b, int field, const void *data, int len)
{
	pb_tag(b, field, PB_LEN);
	pb_varint(b, (uint64) len);
	appendBinaryStringInfo(b, data, len);
}

/* proto3 strings must be UTF-8; replace any invalid byte with '?'. */
static void
pb_string(StringInfo b, int field, const char *s, int len)
{
	StringInfoData clean;
	int			off = 0;

	initStringInfo(&clean);
	while (off < len)
	{
		int			ok = pg_encoding_verifymbstr(PG_UTF8, s + off, len - off);

		appendBinaryStringInfo(&clean, s + off, ok);
		off += ok;
		if (off < len)
		{
			appendStringInfoChar(&clean, '?');
			off++;
		}
	}
	pb_bytes(b, field, clean.data, clean.len);
	pfree(clean.data);
}

static void
pb_message(StringInfo b, int field, StringInfo msg)
{
	pb_bytes(b, field, msg->data, msg->len);
}

/* KeyValue { key = 1; AnyValue value = 2; } */
static void
kv_begin(StringInfo kv, const char *key)
{
	resetStringInfo(kv);
	pb_string(kv, 1, key, (int) strlen(key));
}

static void
kv_end(StringInfo dst, int field, StringInfo kv, StringInfo any)
{
	pb_message(kv, 2, any);
	pb_message(dst, field, kv);
}

static void
attr_str(StringInfo dst, int field, const char *key, const char *val, int len)
{
	StringInfoData kv,
				any;

	initStringInfo(&kv);
	initStringInfo(&any);
	kv_begin(&kv, key);
	pb_string(&any, ANY_STRING, val, len);
	kv_end(dst, field, &kv, &any);
	pfree(kv.data);
	pfree(any.data);
}

static void
attr_int(StringInfo dst, int field, const char *key, int64 val)
{
	StringInfoData kv,
				any;

	initStringInfo(&kv);
	initStringInfo(&any);
	kv_begin(&kv, key);
	pb_uint(&any, ANY_INT, (uint64) val);
	kv_end(dst, field, &kv, &any);
	pfree(kv.data);
	pfree(any.data);
}

#define SPAN_ATTR_STR(b, k, v) attr_str((b), 9, (k), (v), (int) strlen(v))
#define SPAN_ATTR_INT(b, k, v) attr_int((b), 9, (k), (v))

static const char *
operation_name(uint8 cmd)
{
	switch ((CmdType) cmd)
	{
		case CMD_SELECT:
			return "SELECT";
		case CMD_UPDATE:
			return "UPDATE";
		case CMD_INSERT:
			return "INSERT";
		case CMD_DELETE:
			return "DELETE";
#if PG_VERSION_NUM >= 150000
		case CMD_MERGE:
			return "MERGE";
#endif
		default:
			return NULL;
	}
}

static uint64
unix_nanos(TimestampTz ts)
{
	return (uint64) (ts + PG_TO_UNIX_US) * 1000;
}

static void
random_bytes(uint8 *buf, int n)
{
	if (!pg_strong_random(buf, n))
	{
		int			i;

		for (i = 0; i < n; i++)
			buf[i] = (uint8) random();
	}
}

static bool
all_zero(const uint8 *p, int n)
{
	while (n-- > 0)
		if (*p++)
			return false;
	return true;
}

/*
 * Append one Span (as ScopeSpans.spans, field 2) to 'spans'.
 *
 * Attributes follow OpenTelemetry database semantic conventions
 * (db.system.name, db.namespace, db.operation.name, db.collection.name,
 * db.query.summary, db.query.text, db.response.returned_rows,
 * db.response.status_code, error.type).  Anything Postgres-specific that
 * semconv does not define lives under "postgresql.".
 */
void
otel_otlp_append_span(StringInfo spans, const OtelSpan *s)
{
	StringInfoData sp,
				scratch;
	uint8		trace_id[16];
	uint8		span_id[8];
	const char *op = operation_name(s->cmd_type);
	const char *query = s->data + s->error_len;
	const char *plan = query + s->query_len;
	char		name[2 * NAMEDATALEN + 16];

	initStringInfo(&sp);

	if (s->span_flags & OTEL_SPAN_HAS_PARENT)
		memcpy(trace_id, s->trace_id, 16);
	else
		random_bytes(trace_id, 16);
	if (all_zero(trace_id, 16))
		trace_id[15] = 1;
	random_bytes(span_id, 8);
	if (all_zero(span_id, 8))
		span_id[7] = 1;

	pb_bytes(&sp, 1, trace_id, 16);
	pb_bytes(&sp, 2, span_id, 8);
	if (s->span_flags & OTEL_SPAN_HAS_PARENT)
		pb_bytes(&sp, 4, s->parent_span_id, 8);

	/* Span name: "{operation} {collection}" else "{operation} {namespace}". */
	if (op == NULL)
		op = "QUERY";
	snprintf(name, sizeof(name), "%s %s", op,
			 s->collection[0] ? s->collection : s->dbname);
	pb_string(&sp, 5, name, (int) strlen(name));

	/*
	 * SERVER: this is the database's side of the client's CLIENT span.  The
	 * db.* attributes are the same ones semconv defines for client spans.
	 */
	pb_uint(&sp, 6, SPAN_KIND_SERVER);
	pb_fixed64(&sp, 7, unix_nanos(s->start_ts));
	pb_fixed64(&sp, 8, unix_nanos(s->end_ts));

	SPAN_ATTR_STR(&sp, "db.system.name", "postgresql");
	if (s->dbname[0])
		SPAN_ATTR_STR(&sp, "db.namespace", s->dbname);
	SPAN_ATTR_STR(&sp, "db.operation.name", op);
	if (s->collection[0])
		SPAN_ATTR_STR(&sp, "db.collection.name", s->collection);
	if (s->collection[0])
	{
		snprintf(name, sizeof(name), "%s %s", op, s->collection);
		SPAN_ATTR_STR(&sp, "db.query.summary", name);
	}
	else
		SPAN_ATTR_STR(&sp, "db.query.summary", op);

	if (s->query_len > 0)
	{
		initStringInfo(&scratch);
		if (s->span_flags & OTEL_SPAN_TEXT_VERBATIM)
			appendBinaryStringInfo(&scratch, query, s->query_len);
		else
			otel_redact(&scratch, query, (int) s->query_len, true);
		attr_str(&sp, 9, "db.query.text", scratch.data, scratch.len);
		pfree(scratch.data);
	}

	if (s->cmd_type == CMD_SELECT)
		SPAN_ATTR_INT(&sp, "db.response.returned_rows", (int64) s->rows);
	else
		SPAN_ATTR_INT(&sp, "postgresql.rows.affected", (int64) s->rows);

	SPAN_ATTR_INT(&sp, "postgresql.backend.pid", s->pid);
	if (s->query_id != 0)
		SPAN_ATTR_INT(&sp, "postgresql.query.id", (int64) s->query_id);

	if (s->shared_blks_hit)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.shared.hit", s->shared_blks_hit);
	if (s->shared_blks_read)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.shared.read", s->shared_blks_read);
	if (s->shared_blks_dirtied)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.shared.dirtied", s->shared_blks_dirtied);
	if (s->shared_blks_written)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.shared.written", s->shared_blks_written);
	if (s->temp_blks_read)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.temp.read", s->temp_blks_read);
	if (s->temp_blks_written)
		SPAN_ATTR_INT(&sp, "postgresql.buffers.temp.written", s->temp_blks_written);
	if (s->wal_records)
		SPAN_ATTR_INT(&sp, "postgresql.wal.records", s->wal_records);
	if (s->wal_bytes)
		SPAN_ATTR_INT(&sp, "postgresql.wal.bytes", (int64) s->wal_bytes);

	if (s->plan_len > 0)
	{
		initStringInfo(&scratch);
		if (s->span_flags & OTEL_SPAN_TEXT_VERBATIM)
			appendBinaryStringInfo(&scratch, plan, s->plan_len);
		else
			otel_redact(&scratch, plan, (int) s->plan_len, false);
		attr_str(&sp, 9, "postgresql.query.plan", scratch.data, scratch.len);
		pfree(scratch.data);
		SPAN_ATTR_STR(&sp, "postgresql.query.plan.format", "text");
		attr_int(&sp, 9, "postgresql.query.plan.analyzed",
				 (s->span_flags & OTEL_SPAN_PLAN_ANALYZED) ? 1 : 0);
	}

	if (s->span_flags & OTEL_SPAN_ERROR)
	{
		StringInfoData status;

		SPAN_ATTR_STR(&sp, "error.type", s->sqlstate);
		SPAN_ATTR_STR(&sp, "db.response.status_code", s->sqlstate);

		/* Status { message = 2; code = 3; } */
		initStringInfo(&status);
		if (s->error_len > 0)
			pb_string(&status, 2, s->data, (int) s->error_len);
		pb_uint(&status, 3, OTLP_STATUS_ERROR);
		pb_message(&sp, 15, &status);
		pfree(status.data);
	}

	pb_message(spans, 2, &sp);
	pfree(sp.data);
}

/*
 * ExportTraceServiceRequest {
 *   ResourceSpans { resource { attributes }, scope_spans { scope, spans } }
 * }
 */
void
otel_otlp_build_request(StringInfo out, const StringInfo spans,
						const char *service_name, const char *service_version)
{
	StringInfoData resource,
				scope,
				scope_spans,
				resource_spans;

	initStringInfo(&resource);
	attr_str(&resource, 1, "service.name", service_name, (int) strlen(service_name));
	if (service_version && service_version[0])
		attr_str(&resource, 1, "service.version", service_version, (int) strlen(service_version));

	initStringInfo(&scope);
	pb_string(&scope, 1, OTEL_SCOPE_NAME, (int) strlen(OTEL_SCOPE_NAME));
	pb_string(&scope, 2, OTEL_VERSION, (int) strlen(OTEL_VERSION));

	initStringInfo(&scope_spans);
	pb_message(&scope_spans, 1, &scope);
	appendBinaryStringInfo(&scope_spans, spans->data, spans->len);

	initStringInfo(&resource_spans);
	pb_message(&resource_spans, 1, &resource);
	pb_message(&resource_spans, 2, &scope_spans);

	pb_message(out, 1, &resource_spans);

	pfree(resource.data);
	pfree(scope.data);
	pfree(scope_spans.data);
	pfree(resource_spans.data);
}
