/*-------------------------------------------------------------------------
 * otel.h
 *	  Shared declarations for opentelemetry-postgres.
 *
 * Architecture (see README.md):
 *
 *   backend (hot path)                    background worker (cold path)
 *   ------------------                    -----------------------------
 *   executor hooks                        drains the ring
 *     parse sqlcommenter traceparent      generates span ids
 *     capture timings / counters          sanitizes query text
 *     memcpy one fixed record  ───────►   encodes OTLP protobuf
 *     into a lock-free ring               POSTs to a collector
 *-------------------------------------------------------------------------
 */
#ifndef OTEL_H
#define OTEL_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "port/atomics.h"

#define OTEL_VERSION "0.1.0"
#define OTEL_SCOPE_NAME "opentelemetry-postgres"

/* ------------------------------------------------------------------ */
/* GUCs                                                               */
/* ------------------------------------------------------------------ */

typedef enum OtelQueryTextMode
{
	OTEL_QUERY_TEXT_OFF,
	OTEL_QUERY_TEXT_SANITIZED,
	OTEL_QUERY_TEXT_FULL
} OtelQueryTextMode;

typedef enum OtelPlanMode
{
	OTEL_PLAN_OFF,
	OTEL_PLAN_ESTIMATE,			/* EXPLAIN (no execution statistics) */
	OTEL_PLAN_ANALYZE			/* EXPLAIN ANALYZE, row counts, no timing */
} OtelPlanMode;

extern bool otel_enabled;
extern bool otel_trace_untraced;
extern int	otel_min_duration;	/* ms */
extern int	otel_query_text;	/* OtelQueryTextMode */
extern int	otel_plan_mode;		/* OtelPlanMode */
extern int	otel_plan_min_duration; /* ms, -1 disables */
extern int	otel_buffer_slots;
extern int	otel_slot_size;
extern int	otel_flush_interval;	/* ms */
extern int	otel_max_batch;
extern int	otel_export_timeout;	/* ms */
extern char *otel_endpoint;
extern char *otel_service_name;

/* ------------------------------------------------------------------ */
/* Span record: the only thing that crosses the backend/worker line.  */
/* ------------------------------------------------------------------ */

#define OTEL_SPAN_HAS_PARENT	0x01
#define OTEL_SPAN_ERROR			0x02
#define OTEL_SPAN_PLAN_ANALYZED	0x04
#define OTEL_SPAN_TEXT_VERBATIM	0x08	/* otel.query_text = full in the backend */

typedef struct OtelSpan
{
	uint8		trace_id[16];	/* all zero: worker generates a new trace */
	uint8		parent_span_id[8];
	uint8		trace_flags;	/* W3C trace-flags of the parent */
	uint8		span_flags;		/* OTEL_SPAN_* */
	uint8		cmd_type;		/* CmdType */
	char		sqlstate[6];	/* set when OTEL_SPAN_ERROR */
	int32		pid;
	TimestampTz start_ts;
	TimestampTz end_ts;
	uint64		rows;
	uint64		query_id;
	int64		shared_blks_hit;
	int64		shared_blks_read;
	int64		shared_blks_dirtied;
	int64		shared_blks_written;
	int64		temp_blks_read;
	int64		temp_blks_written;
	int64		wal_records;
	uint64		wal_bytes;
	char		dbname[NAMEDATALEN];
	char		collection[NAMEDATALEN];	/* "" when unknown */
	uint32		query_len;
	uint32		error_len;
	uint32		plan_len;
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* query | error | plan */
} OtelSpan;

typedef struct OtelSlot
{
	pg_atomic_uint64 seq;
	OtelSpan	span;
} OtelSlot;

/* ------------------------------------------------------------------ */
/* Shared memory                                                      */
/* ------------------------------------------------------------------ */

typedef struct OtelShared
{
	pg_atomic_uint64 enqueue_pos;
	char		pad1[PG_CACHE_LINE_SIZE];
	pg_atomic_uint64 dequeue_pos;
	char		pad2[PG_CACHE_LINE_SIZE];
	pg_atomic_uint64 dropped;	/* ring full */
	pg_atomic_uint64 exported;
	pg_atomic_uint64 export_failures;	/* failed HTTP exports */
	pg_atomic_uint64 export_dropped;	/* spans lost to failed exports */
	pg_atomic_uint32 wake_pending;
	pg_atomic_uint32 worker_procno_plus1;
	uint32		nslots;			/* power of two */
	uint32		slot_stride;	/* bytes between slots */
	/* slots follow, MAXALIGNed */
} OtelShared;

extern OtelShared *otel_shared;

extern Size otel_shmem_size(void);
extern void otel_shmem_init(void);

/* Ring: many producers (backends), one consumer (worker). */
extern OtelSlot *otel_queue_claim(uint64 *pos);
extern void otel_queue_publish(OtelSlot *slot, uint64 pos);
extern OtelSlot *otel_queue_peek(uint64 *pos);
extern void otel_queue_release(OtelSlot *slot, uint64 pos);
extern Size otel_slot_data_capacity(void);

/* ------------------------------------------------------------------ */
/* Components                                                         */
/* ------------------------------------------------------------------ */

extern void otel_hooks_install(void);
extern void otel_worker_register(void);
extern PGDLLEXPORT void otel_worker_main(Datum arg);

/* sanitize: redact literals. sql_mode additionally strips comments,
 * redacts numbers and collapses whitespace. */
extern void otel_redact(StringInfo out, const char *s, int len, bool sql_mode);

/* traceparent */
typedef struct OtelTraceCtx
{
	uint8		trace_id[16];
	uint8		span_id[8];
	uint8		flags;
} OtelTraceCtx;

typedef enum OtelParentResult
{
	OTEL_PARENT_NONE,
	OTEL_PARENT_SAMPLED,
	OTEL_PARENT_NOT_SAMPLED
} OtelParentResult;

extern OtelParentResult otel_find_traceparent(const char *s, int len, OtelTraceCtx *ctx);

/* OTLP */
extern void otel_otlp_append_span(StringInfo spans, const OtelSpan *s);
extern void otel_otlp_build_request(StringInfo out, const StringInfo spans,
									const char *service_name,
									const char *service_version);

/* HTTP */
extern bool otel_http_post(const char *url, const char *body, int len,
						   int timeout_ms, char *err, int errlen);

#endif							/* OTEL_H */
