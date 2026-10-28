/*-------------------------------------------------------------------------
 * otel_hooks.c
 *	  Executor hooks: the only code that runs on the query path.
 *
 * Budget per traced query: a bounded scan for "traceparent='" in the query
 * text, two clock reads, two struct copies (buffer / WAL counters), and one
 * memcpy of a few hundred bytes into the ring.  Everything else (span ids,
 * sanitising, protobuf, network) happens in the background worker.
 *
 * Queries that do not qualify for tracing cost one bounded text scan.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "commands/explain.h"
#include "commands/dbcommands.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "otel.h"

#if PG_VERSION_NUM >= 180000
#error "PostgreSQL 18 changed the ExecutorStart hook signature; not yet supported"
#endif

/*
 * Backend-local table of in-flight traced queries, keyed by QueryDesc.
 * Entries are released by a reset callback on the executor's memory context,
 * which fires on normal end, on error and on abort alike.
 */
#define OTEL_MAX_INFLIGHT 16

typedef struct OtelQueryState
{
	bool		in_use;
	bool		done;			/* span already emitted (error path) */
	QueryDesc  *qd;
	TimestampTz start;
	BufferUsage buf0;
	WalUsage	wal0;
	OtelParentResult parent;
	OtelTraceCtx ctx;
	char		collection[NAMEDATALEN];
} OtelQueryState;

static OtelQueryState inflight[OTEL_MAX_INFLIGHT];

/* Database name, looked up once per backend (a backend never changes database). */
static char backend_dbname[NAMEDATALEN];

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* ------------------------------------------------------------------ */
/* traceparent                                                        */
/* ------------------------------------------------------------------ */

/* Only the head and tail of the query are scanned: sqlcommenter puts its
 * comment at one end, and this bounds the cost for very large statements. */
#define OTEL_SCAN_WINDOW 1024

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;					/* W3C: lowercase only */
}

static bool
parse_hex(const char *s, int nbytes, uint8 *out)
{
	bool		nonzero = false;
	int			i;

	for (i = 0; i < nbytes; i++)
	{
		int			hi = hexval(s[2 * i]);
		int			lo = hexval(s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		out[i] = (uint8) (hi << 4 | lo);
		nonzero |= out[i] != 0;
	}
	return nonzero;
}

/* Is p inside a block comment that was opened within the preceding window? */
static bool
inside_block_comment(const char *base, const char *p)
{
	const char *lo = (p - base > OTEL_SCAN_WINDOW) ? p - OTEL_SCAN_WINDOW : base;
	const char *q;

	for (q = p - 1; q > lo; q--)
	{
		if (q[-1] == '*' && q[0] == '/')
			return false;		/* a comment closed before reaching p */
		if (q[-1] == '/' && q[0] == '*')
			return true;
	}
	return false;
}

static bool
scan_window(const char *base, const char *start, const char *end, OtelTraceCtx *ctx,
			bool *sampled)
{
	static const char key[] = "traceparent='";
	const int	keylen = sizeof(key) - 1;
	const int	vallen = 55;	/* 00-<32>-<16>-<2> */
	const char *p = start;

	while (end - p >= keylen + vallen + 1 &&
		   (p = memchr(p, 't', end - p - (keylen + vallen))) != NULL)
	{
		if (memcmp(p, key, keylen) == 0)
		{
			const char *v = p + keylen;
			uint8		flags;

			if (v[vallen] == '\'' &&
				v[2] == '-' && v[35] == '-' && v[52] == '-' &&
				!(v[0] == 'f' && v[1] == 'f') &&
				parse_hex(v + 3, 16, ctx->trace_id) &&
				parse_hex(v + 36, 8, ctx->span_id) &&
				(hexval(v[53]) >= 0 && hexval(v[54]) >= 0) &&
				inside_block_comment(base, p))
			{
				flags = (uint8) (hexval(v[53]) << 4 | hexval(v[54]));
				ctx->flags = flags;
				*sampled = (flags & 0x01) != 0;
				return true;
			}
		}
		p++;
	}
	return false;
}

OtelParentResult
otel_find_traceparent(const char *s, int len, OtelTraceCtx *ctx)
{
	bool		sampled = false;

	if (len <= 2 * OTEL_SCAN_WINDOW)
	{
		if (!scan_window(s, s, s + len, ctx, &sampled))
			return OTEL_PARENT_NONE;
	}
	else if (!scan_window(s, s, s + OTEL_SCAN_WINDOW, ctx, &sampled) &&
			 !scan_window(s, s + len - OTEL_SCAN_WINDOW, s + len, ctx, &sampled))
		return OTEL_PARENT_NONE;

	return sampled ? OTEL_PARENT_SAMPLED : OTEL_PARENT_NOT_SAMPLED;
}

/* ------------------------------------------------------------------ */
/* In-flight table                                                    */
/* ------------------------------------------------------------------ */

static OtelQueryState *
find_state(QueryDesc *qd)
{
	int			i;

	for (i = 0; i < OTEL_MAX_INFLIGHT; i++)
		if (inflight[i].in_use && inflight[i].qd == qd)
			return &inflight[i];
	return NULL;
}

static void
state_release(void *arg)
{
	((OtelQueryState *) arg)->in_use = false;
}

/*
 * Name of the single table a statement touches, if it is unambiguous: the
 * target for INSERT/UPDATE/DELETE/MERGE, or the only relation of a SELECT.
 */
static void
resolve_collection(QueryDesc *qd, char *out)
{
	PlannedStmt *pstmt = qd->plannedstmt;
	Oid			relid = InvalidOid;

	out[0] = '\0';

	if (pstmt->commandType == CMD_SELECT)
	{
		ListCell   *lc;

		foreach(lc, pstmt->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			if (rte->rtekind != RTE_RELATION)
				continue;
			if (OidIsValid(relid) && relid != rte->relid)
				return;			/* more than one relation */
			relid = rte->relid;
		}
	}
	else if (pstmt->resultRelations != NIL)
	{
		Index		rti = linitial_int(pstmt->resultRelations);

		relid = rt_fetch(rti, pstmt->rtable)->relid;
	}

	if (OidIsValid(relid))
	{
		char	   *name = get_rel_name(relid);

		if (name)
		{
			strlcpy(out, name, NAMEDATALEN);
			pfree(name);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Emit                                                               */
/* ------------------------------------------------------------------ */

static const char *
statement_slice(QueryDesc *qd, int *len)
{
	PlannedStmt *pstmt = qd->plannedstmt;
	const char *src = qd->sourceText;
	int			total = (int) strlen(src);
	int			loc = pstmt->stmt_location;
	int			l = pstmt->stmt_len;

	if (loc < 0 || loc > total)
		loc = 0;
	if (l <= 0 || loc + l > total)
		l = total - loc;
	*len = l;
	return src + loc;
}

static char *
build_plan(QueryDesc *qd)
{
	ExplainState *es = NewExplainState();
	MemoryContext oldcxt = MemoryContextSwitchTo(qd->estate->es_query_cxt);
	char	   *plan;

	es->analyze = (otel_plan_mode == OTEL_PLAN_ANALYZE) && qd->instrument_options != 0;
	es->timing = false;
	es->format = EXPLAIN_FORMAT_TEXT;
	ExplainBeginOutput(es);
	ExplainPrintPlan(es, qd);
	ExplainEndOutput(es);
	if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
		es->str->data[--es->str->len] = '\0';
	plan = es->str->data;
	MemoryContextSwitchTo(oldcxt);
	return plan;
}

/*
 * Copy up to 'cap' bytes of 'src' (clipped on a character boundary) to *dst.
 */
static uint32
copy_clipped(char **dst, const char *src, int len, int cap)
{
	int			n = pg_mbcliplen(src, len, cap);

	memcpy(*dst, src, n);
	*dst += n;
	return (uint32) n;
}

static void
emit_span(OtelQueryState *st, QueryDesc *qd, const char *sqlstate, const char *errmsg)
{
	TimestampTz end = GetCurrentTimestamp();
	bool		failed = sqlstate != NULL;
	OtelSlot   *slot;
	OtelSpan   *sp;
	uint64		pos;
	char	   *plan = NULL;
	int			planlen = 0;
	const char *query = NULL;
	int			qlen = 0;
	int			errlen = 0;
	Size		cap;
	char	   *w;
	BufferUsage bu;
	WalUsage	wu;

	st->done = true;

	if (!failed && (end - st->start) < (int64) otel_min_duration * 1000)
		return;

	/* Plans are the one expensive thing we do in-process; gate hard. */
	if (!failed && otel_plan_mode != OTEL_PLAN_OFF && otel_plan_min_duration >= 0 &&
		(end - st->start) >= (int64) otel_plan_min_duration * 1000)
	{
		plan = build_plan(qd);
		planlen = (int) strlen(plan);
	}

	if (otel_query_text != OTEL_QUERY_TEXT_OFF)
		query = statement_slice(qd, &qlen);
	if (failed && errmsg && otel_query_text == OTEL_QUERY_TEXT_FULL)
		errlen = (int) strlen(errmsg);

	slot = otel_queue_claim(&pos);
	if (slot == NULL)
		return;

	sp = &slot->span;
	cap = otel_slot_data_capacity();
	w = sp->data;

	/* Budget: error <= 256 B, query <= half of the rest, plan gets the remainder. */
	sp->error_len = copy_clipped(&w, errmsg ? errmsg : "", errlen, Min(256, cap / 8));
	cap -= sp->error_len;
	sp->query_len = copy_clipped(&w, query ? query : "", qlen, cap / 2);
	cap -= sp->query_len;
	sp->plan_len = copy_clipped(&w, plan ? plan : "", planlen, cap);

	memcpy(sp->trace_id, st->ctx.trace_id, 16);
	memcpy(sp->parent_span_id, st->ctx.span_id, 8);
	sp->trace_flags = st->ctx.flags;
	sp->span_flags = 0;
	if (st->parent != OTEL_PARENT_NONE)
		sp->span_flags |= OTEL_SPAN_HAS_PARENT;
	if (failed)
	{
		sp->span_flags |= OTEL_SPAN_ERROR;
		strlcpy(sp->sqlstate, sqlstate, sizeof(sp->sqlstate));
	}
	if (otel_query_text == OTEL_QUERY_TEXT_FULL)
		sp->span_flags |= OTEL_SPAN_TEXT_VERBATIM;
	if (plan && otel_plan_mode == OTEL_PLAN_ANALYZE)
		sp->span_flags |= OTEL_SPAN_PLAN_ANALYZED;
	sp->cmd_type = (uint8) qd->operation;
	sp->pid = MyProcPid;
	sp->start_ts = st->start;
	sp->end_ts = end;
	sp->rows = qd->estate ? qd->estate->es_processed : 0;
	sp->query_id = (uint64) qd->plannedstmt->queryId;

	bu = pgBufferUsage;
	wu = pgWalUsage;
	sp->shared_blks_hit = bu.shared_blks_hit - st->buf0.shared_blks_hit;
	sp->shared_blks_read = bu.shared_blks_read - st->buf0.shared_blks_read;
	sp->shared_blks_dirtied = bu.shared_blks_dirtied - st->buf0.shared_blks_dirtied;
	sp->shared_blks_written = bu.shared_blks_written - st->buf0.shared_blks_written;
	sp->temp_blks_read = bu.temp_blks_read - st->buf0.temp_blks_read;
	sp->temp_blks_written = bu.temp_blks_written - st->buf0.temp_blks_written;
	sp->wal_records = wu.wal_records - st->wal0.wal_records;
	sp->wal_bytes = wu.wal_bytes - st->wal0.wal_bytes;

	strlcpy(sp->dbname, backend_dbname, NAMEDATALEN);
	strlcpy(sp->collection, st->collection, NAMEDATALEN);

	otel_queue_publish(slot, pos);
}

/* ------------------------------------------------------------------ */
/* Hooks                                                              */
/* ------------------------------------------------------------------ */

static void
otel_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	OtelQueryState local;
	bool		trace = false;

	if (otel_enabled && otel_shared != NULL && MyProcPort != NULL &&
		!IsParallelWorker() && queryDesc->sourceText != NULL &&
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
	{
		const char *src = queryDesc->sourceText;

		memset(&local.ctx, 0, sizeof(local.ctx));
		local.parent = otel_find_traceparent(src, (int) strlen(src), &local.ctx);
		trace = local.parent == OTEL_PARENT_SAMPLED ||
			(local.parent == OTEL_PARENT_NONE && otel_trace_untraced);
	}

	if (trace)
	{
		local.start = GetCurrentTimestamp();
		local.buf0 = pgBufferUsage;
		local.wal0 = pgWalUsage;
		if (otel_plan_mode == OTEL_PLAN_ANALYZE && otel_plan_min_duration >= 0)
			queryDesc->instrument_options |= INSTRUMENT_ROWS;
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (trace)
	{
		int			i;

		for (i = 0; i < OTEL_MAX_INFLIGHT; i++)
		{
			OtelQueryState *st = &inflight[i];
			MemoryContextCallback *cb;

			if (st->in_use)
				continue;

			if (backend_dbname[0] == '\0')
			{
				char	   *dbname = get_database_name(MyDatabaseId);

				if (dbname)
				{
					strlcpy(backend_dbname, dbname, NAMEDATALEN);
					pfree(dbname);
				}
			}

			*st = local;
			st->qd = queryDesc;
			st->done = false;
			resolve_collection(queryDesc, st->collection);

			/* Released with the executor's context: covers error and abort. */
			cb = MemoryContextAlloc(queryDesc->estate->es_query_cxt,
									sizeof(MemoryContextCallback));
			cb->func = state_release;
			cb->arg = st;
			MemoryContextRegisterResetCallback(queryDesc->estate->es_query_cxt, cb);
			st->in_use = true;
			break;
		}
	}
}

static void
otel_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	OtelQueryState *st = find_state(queryDesc);
	MemoryContext oldcxt = CurrentMemoryContext;

	if (st == NULL)
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		return;
	}

	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
	}
	PG_CATCH();
	{
		/* ExecutorEnd never runs for a failed query: emit the span here. */
		ErrorData  *edata;
		char		sqlstate[6];

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		strlcpy(sqlstate, unpack_sql_state(edata->sqlerrcode), sizeof(sqlstate));
		if (!st->done)
			emit_span(st, queryDesc, sqlstate, edata->message);
		FreeErrorData(edata);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
otel_ExecutorEnd(QueryDesc *queryDesc)
{
	OtelQueryState *st = find_state(queryDesc);

	if (st != NULL && !st->done)
		emit_span(st, queryDesc, NULL, NULL);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
otel_hooks_install(void)
{
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = otel_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = otel_ExecutorRun;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = otel_ExecutorEnd;
}
