/*-------------------------------------------------------------------------
 * otel_worker.c
 *	  Background worker: drains the span ring and exports OTLP/HTTP.
 *
 * It never connects to a database, so it holds no snapshots or locks and
 * costs nothing but its own CPU.  It sleeps on its latch for
 * otel.flush_interval and is woken early by backends when the ring is half
 * full.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 150000
#include "utils/wait_event.h"
#else
#include "pgstat.h"
#endif

#include "otel.h"

#define OTEL_DEFAULT_ENDPOINT "http://localhost:4318/v1/traces"

void
otel_worker_register(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;	/* also on standbys */
	worker.bgw_restart_time = 10;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "opentelemetry_postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "otel_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "opentelemetry-postgres exporter");
	snprintf(worker.bgw_type, BGW_MAXLEN, "opentelemetry-postgres exporter");
	RegisterBackgroundWorker(&worker);
}

static const char *
resolve_endpoint(char *buf, int buflen)
{
	const char *e;

	if (otel_endpoint && otel_endpoint[0])
		return otel_endpoint;
	if ((e = getenv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT")) && e[0])
		return e;
	if ((e = getenv("OTEL_EXPORTER_OTLP_ENDPOINT")) && e[0])
	{
		int			n = (int) strlen(e);

		snprintf(buf, buflen, "%.*s/v1/traces", (e[n - 1] == '/') ? n - 1 : n, e);
		return buf;
	}
	return OTEL_DEFAULT_ENDPOINT;
}

static const char *
resolve_service_name(void)
{
	const char *e;

	if (otel_service_name && otel_service_name[0])
		return otel_service_name;
	if ((e = getenv("OTEL_SERVICE_NAME")) && e[0])
		return e;
	return "postgresql";
}

/*
 * Export one batch.  Returns the number of spans taken from the ring (0 when
 * it was empty).  A batch that cannot be delivered is dropped and counted: the
 * worker must never let a dead collector turn into unbounded memory.
 */
static int
export_batch(MemoryContext batchcxt)
{
	StringInfoData spans,
				request;
	char		epbuf[512],
				err[256];
	int			n = 0;
	bool		ok;

	initStringInfo(&spans);

	while (n < otel_max_batch)
	{
		uint64		pos;
		OtelSlot   *slot = otel_queue_peek(&pos);

		if (slot == NULL)
			break;
		otel_otlp_append_span(&spans, &slot->span);
		otel_queue_release(slot, pos);
		n++;
	}

	if (n == 0)
	{
		MemoryContextReset(batchcxt);
		return 0;
	}

	initStringInfo(&request);
	otel_otlp_build_request(&request, &spans, resolve_service_name(),
							GetConfigOption("server_version", true, false));

	ok = otel_http_post(resolve_endpoint(epbuf, sizeof(epbuf)), request.data,
						request.len, otel_export_timeout, err, sizeof(err));
	if (ok)
		pg_atomic_fetch_add_u64(&otel_shared->exported, n);
	else
	{
		static TimestampTz last_log = 0;
		TimestampTz now = GetCurrentTimestamp();

		pg_atomic_fetch_add_u64(&otel_shared->export_failures, 1);
		pg_atomic_fetch_add_u64(&otel_shared->export_dropped, n);
		if (now - last_log > 60 * USECS_PER_SEC)	/* rate-limit the log */
		{
			ereport(WARNING,
					(errmsg("opentelemetry-postgres: dropped %d spans: %s", n, err)));
			last_log = now;
		}
	}

	MemoryContextReset(batchcxt);
	return n;
}

static void
drain(MemoryContext batchcxt)
{
	MemoryContext old = MemoryContextSwitchTo(batchcxt);

	while (export_batch(batchcxt) > 0)
		CHECK_FOR_INTERRUPTS();
	MemoryContextSwitchTo(old);
}

void
otel_worker_main(Datum arg)
{
	MemoryContext batchcxt;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

#if PG_VERSION_NUM >= 170000
	pg_atomic_write_u32(&otel_shared->worker_procno_plus1, MyProcNumber + 1);
#else
	pg_atomic_write_u32(&otel_shared->worker_procno_plus1, MyProc->pgprocno + 1);
#endif

	batchcxt = AllocSetContextCreate(TopMemoryContext, "opentelemetry-postgres batch",
									 ALLOCSET_DEFAULT_SIZES);

	ereport(LOG, (errmsg("opentelemetry-postgres exporter started")));

	while (!ShutdownRequestPending)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 otel_flush_interval, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		pg_atomic_write_u32(&otel_shared->wake_pending, 0);

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		drain(batchcxt);
	}

	/* Final flush on shutdown. */
	drain(batchcxt);
	pg_atomic_write_u32(&otel_shared->worker_procno_plus1, 0);
	proc_exit(0);
}
