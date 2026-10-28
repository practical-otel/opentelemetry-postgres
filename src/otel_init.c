/*-------------------------------------------------------------------------
 * otel_init.c
 *	  Module entry point, GUCs, shared memory and the span ring.
 *
 * The ring is a bounded multi-producer / single-consumer queue in the style
 * of Dmitry Vyukov's bounded MPMC queue: every slot carries a sequence number
 * that says whether it is free for the producer at position N or ready for the
 * consumer at position N.  Producers never block and never take a lock; when
 * the ring is full the span is dropped and counted.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"

#include "otel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(otel_stats);

/* GUC storage */
bool		otel_enabled = true;
bool		otel_trace_untraced = false;
int			otel_min_duration = 0;
int			otel_query_text = OTEL_QUERY_TEXT_SANITIZED;
int			otel_plan_mode = OTEL_PLAN_OFF;
int			otel_plan_min_duration = 100;
int			otel_buffer_slots = 2048;
int			otel_slot_size = 4096;
int			otel_flush_interval = 1000;
int			otel_max_batch = 512;
int			otel_export_timeout = 5000;
char	   *otel_endpoint = NULL;
char	   *otel_service_name = NULL;

OtelShared *otel_shared = NULL;

#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static const struct config_enum_entry query_text_options[] = {
	{"off", OTEL_QUERY_TEXT_OFF, false},
	{"sanitized", OTEL_QUERY_TEXT_SANITIZED, false},
	{"full", OTEL_QUERY_TEXT_FULL, false},
	{NULL, 0, false}
};

static const struct config_enum_entry plan_mode_options[] = {
	{"off", OTEL_PLAN_OFF, false},
	{"estimate", OTEL_PLAN_ESTIMATE, false},
	{"analyze", OTEL_PLAN_ANALYZE, false},
	{NULL, 0, false}
};

/* ------------------------------------------------------------------ */
/* Shared memory                                                      */
/* ------------------------------------------------------------------ */

static uint32
effective_nslots(void)
{
	return pg_nextpower2_32((uint32) otel_buffer_slots);
}

static uint32
effective_stride(void)
{
	return (uint32) MAXALIGN(otel_slot_size);
}

Size
otel_shmem_size(void)
{
	return MAXALIGN(sizeof(OtelShared)) +
		mul_size(effective_nslots(), effective_stride());
}

static inline OtelSlot *
slot_at(uint64 pos)
{
	char	   *base = (char *) otel_shared + MAXALIGN(sizeof(OtelShared));

	return (OtelSlot *) (base +
						 (size_t) (pos & (otel_shared->nslots - 1)) *
						 otel_shared->slot_stride);
}

Size
otel_slot_data_capacity(void)
{
	return otel_shared->slot_stride - offsetof(OtelSlot, span.data);
}

#if PG_VERSION_NUM >= 150000
static void
otel_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(otel_shmem_size());
}
#endif

static void
otel_shmem_startup(void)
{
	bool		found;
	uint32		i;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	otel_shared = ShmemInitStruct("opentelemetry_postgres", otel_shmem_size(), &found);
	if (!found)
	{
		memset(otel_shared, 0, otel_shmem_size());
		pg_atomic_init_u64(&otel_shared->enqueue_pos, 0);
		pg_atomic_init_u64(&otel_shared->dequeue_pos, 0);
		pg_atomic_init_u64(&otel_shared->dropped, 0);
		pg_atomic_init_u64(&otel_shared->exported, 0);
		pg_atomic_init_u64(&otel_shared->export_failures, 0);
		pg_atomic_init_u64(&otel_shared->export_dropped, 0);
		pg_atomic_init_u32(&otel_shared->wake_pending, 0);
		pg_atomic_init_u32(&otel_shared->worker_procno_plus1, 0);
		otel_shared->nslots = effective_nslots();
		otel_shared->slot_stride = effective_stride();
		for (i = 0; i < otel_shared->nslots; i++)
			pg_atomic_init_u64(&slot_at(i)->seq, i);
	}
	LWLockRelease(AddinShmemInitLock);
}

/* ------------------------------------------------------------------ */
/* Ring                                                               */
/* ------------------------------------------------------------------ */

/*
 * Reserve a slot.  Returns NULL (and counts a drop) when the ring is full.
 * The caller fills slot->span and then calls otel_queue_publish().
 */
OtelSlot *
otel_queue_claim(uint64 *pos_out)
{
	uint64		pos = pg_atomic_read_u64(&otel_shared->enqueue_pos);

	for (;;)
	{
		OtelSlot   *slot = slot_at(pos);
		int64		dif = (int64) (pg_atomic_read_u64(&slot->seq) - pos);

		if (dif == 0)
		{
			if (pg_atomic_compare_exchange_u64(&otel_shared->enqueue_pos,
											   &pos, pos + 1))
			{
				*pos_out = pos;
				return slot;
			}
			/* CAS failure reloaded pos; retry */
		}
		else if (dif < 0)
		{
			pg_atomic_fetch_add_u64(&otel_shared->dropped, 1);
			return NULL;
		}
		else
			pos = pg_atomic_read_u64(&otel_shared->enqueue_pos);
	}
}

void
otel_queue_publish(OtelSlot *slot, uint64 pos)
{
	uint64		depth;

	pg_write_barrier();
	pg_atomic_write_u64(&slot->seq, pos + 1);

	/* Wake the worker early once the ring is half full. */
	depth = pos + 1 - pg_atomic_read_u64(&otel_shared->dequeue_pos);
	if (depth >= otel_shared->nslots / 2 &&
		pg_atomic_exchange_u32(&otel_shared->wake_pending, 1) == 0)
	{
		uint32		procno = pg_atomic_read_u32(&otel_shared->worker_procno_plus1);

		if (procno > 0)
			SetLatch(&ProcGlobal->allProcs[procno - 1].procLatch);
	}
}

/* Consumer side.  Returns NULL when nothing is ready. */
OtelSlot *
otel_queue_peek(uint64 *pos_out)
{
	uint64		pos = pg_atomic_read_u64(&otel_shared->dequeue_pos);
	OtelSlot   *slot = slot_at(pos);

	if (pg_atomic_read_u64(&slot->seq) != pos + 1)
		return NULL;
	pg_read_barrier();
	*pos_out = pos;
	return slot;
}

void
otel_queue_release(OtelSlot *slot, uint64 pos)
{
	pg_memory_barrier();
	pg_atomic_write_u64(&slot->seq, pos + otel_shared->nslots);
	pg_atomic_write_u64(&otel_shared->dequeue_pos, pos + 1);
}

/* ------------------------------------------------------------------ */
/* SQL: otel_stats()                                                  */
/* ------------------------------------------------------------------ */

Datum
otel_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false};
	uint64		enq,
				deq;

	if (otel_shared == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("opentelemetry_postgres must be loaded via shared_preload_libraries")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	enq = pg_atomic_read_u64(&otel_shared->enqueue_pos);
	deq = pg_atomic_read_u64(&otel_shared->dequeue_pos);

	values[0] = Int64GetDatum((int64) enq);
	values[1] = Int64GetDatum((int64) pg_atomic_read_u64(&otel_shared->dropped));
	values[2] = Int64GetDatum((int64) pg_atomic_read_u64(&otel_shared->exported));
	values[3] = Int64GetDatum((int64) pg_atomic_read_u64(&otel_shared->export_failures));
	values[4] = Int64GetDatum((int64) pg_atomic_read_u64(&otel_shared->export_dropped));
	values[5] = Int64GetDatum((int64) (enq - deq));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* ------------------------------------------------------------------ */
/* _PG_init                                                           */
/* ------------------------------------------------------------------ */

void		_PG_init(void);

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("opentelemetry_postgres must be loaded via shared_preload_libraries")));

	DefineCustomBoolVariable("otel.enabled",
							 "Enables tracing of queries.",
							 NULL, &otel_enabled, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("otel.trace_untraced",
							 "Also create root spans for queries without a sqlcommenter traceparent.",
							 "When off (the default) only queries that carry a sampled traceparent are traced.",
							 &otel_trace_untraced, false,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.min_duration",
							"Spans shorter than this are not exported. Failed queries are always exported.",
							NULL, &otel_min_duration, 0, 0, INT_MAX,
							PGC_SUSET, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomEnumVariable("otel.query_text",
							 "How db.query.text is recorded.",
							 "off: not recorded. sanitized: literals redacted. full: verbatim (may contain sensitive data).",
							 &otel_query_text, OTEL_QUERY_TEXT_SANITIZED,
							 query_text_options, PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomEnumVariable("otel.plan_mode",
							 "Attach the query plan to spans.",
							 "estimate: planner output only. analyze: also actual row counts (adds per-node row counting to every traced query).",
							 &otel_plan_mode, OTEL_PLAN_OFF,
							 plan_mode_options, PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.plan_min_duration",
							"Only attach plans to spans at least this long. -1 disables plans.",
							NULL, &otel_plan_min_duration, 100, -1, INT_MAX,
							PGC_SUSET, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.buffer_slots",
							"Number of spans buffered between backends and the exporter (rounded up to a power of two).",
							NULL, &otel_buffer_slots, 2048, 64, 1 << 20,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.slot_size",
							"Size of each buffered span, including query text and plan; longer text is truncated.",
							NULL, &otel_slot_size, 4096, 1024, 1 << 20,
							PGC_POSTMASTER, GUC_UNIT_BYTE, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.flush_interval",
							"Maximum time spans wait in the buffer before export.",
							NULL, &otel_flush_interval, 1000, 10, 3600000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.max_batch",
							"Maximum number of spans per OTLP export request.",
							NULL, &otel_max_batch, 512, 1, 100000,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("otel.export_timeout",
							"Connect/send/receive timeout for OTLP export.",
							NULL, &otel_export_timeout, 5000, 100, 600000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomStringVariable("otel.endpoint",
							   "OTLP/HTTP traces endpoint, e.g. http://localhost:4318/v1/traces.",
							   "Falls back to OTEL_EXPORTER_OTLP_TRACES_ENDPOINT, then OTEL_EXPORTER_OTLP_ENDPOINT, then http://localhost:4318/v1/traces. Only plain http:// is supported.",
							   &otel_endpoint, "",
							   PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("otel.service_name",
							   "service.name resource attribute.",
							   "Falls back to OTEL_SERVICE_NAME, then 'postgresql'.",
							   &otel_service_name, "",
							   PGC_SIGHUP, 0, NULL, NULL, NULL);

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("otel");

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = otel_shmem_request;
#else
	EmitWarningsOnPlaceholders("otel");
	RequestAddinShmemSpace(otel_shmem_size());
#endif
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = otel_shmem_startup;

	otel_hooks_install();
	otel_worker_register();
}
