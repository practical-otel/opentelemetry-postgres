package main

import (
	"strings"
	"testing"
	"time"

	"go.opentelemetry.io/otel/attribute"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
)

const tp1 = `/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01'*/`

// run feeds log text through the whole pipeline and returns the spans produced.
func run(t *testing.T, prefix, logText string, cfg Config) []sdktrace.ReadOnlySpan {
	t.Helper()
	pfx, err := CompilePrefix(prefix)
	if err != nil {
		t.Fatal(err)
	}
	exp := tracetest.NewInMemoryExporter()
	tpv := sdktrace.NewTracerProvider(sdktrace.WithSyncer(exp))
	b := &Builder{tracer: tpv.Tracer("test"), cfg: cfg}
	asm := NewAssembler(b.Handle)
	rd := NewReader(pfx, asm.Handle)
	rd.Feed(logText)
	rd.Flush()
	asm.FlushAll()
	return exp.GetSpans().Snapshots()
}

func attrs(s sdktrace.ReadOnlySpan) map[string]attribute.Value {
	m := map[string]attribute.Value{}
	for _, kv := range s.Attributes() {
		m[string(kv.Key)] = kv.Value
	}
	return m
}

var defaultCfg = Config{QueryText: "sanitized", IncludePlans: true}

// Real output from postgres:17 with auto_explain.log_min_duration=0, log_analyze, log_buffers,
// log_min_duration_statement=0 and log_line_prefix='%t:%r:%u@%d:[%p]:' (the RDS default).
const rdsLog = "2026-10-28 18:42:12 UTC:[local]:postgres@postgres:[80]:LOG:  duration: 0.019 ms  plan:\n" +
	"\tQuery Text: select * from t where id = 1 " + tp1 + "\n" +
	"\tIndex Scan using t_pkey on t  (cost=0.15..8.17 rows=1 width=36) (actual rows=1 loops=1)\n" +
	"\t  Index Cond: (id = 1)\n" +
	"\t  Buffers: shared hit=2\n" +
	"2026-10-28 18:42:12 UTC:[local]:postgres@postgres:[80]:LOG:  duration: 0.894 ms  statement: select * from t where id = 1 " + tp1 + "\n" +
	"2026-10-28 18:42:12 UTC:[local]:postgres@postgres:[87]:ERROR:  division by zero\n" +
	"2026-10-28 18:42:12 UTC:[local]:postgres@postgres:[87]:STATEMENT:  select 1/(id-id) from t " + tp1 + "\n" +
	"2026-10-28 18:42:13 UTC:[local]:postgres@postgres:[90]:LOG:  duration: 0.5 ms  statement: select 2\n"

func TestRealLogProducesParentedMergedSpans(t *testing.T) {
	spans := run(t, "%t:%r:%u@%d:[%p]:", rdsLog, defaultCfg)
	if len(spans) != 2 { // select 2 has no traceparent
		t.Fatalf("want 2 spans, got %d", len(spans))
	}
	sel, errSpan := spans[0], spans[1]

	if sel.Parent().SpanID().String() != "00f067aa0ba902b7" || sel.SpanContext().TraceID().String() != "4bf92f3577b34da6a3ce929d0e0e4736" {
		t.Errorf("not parented to the sqlcommenter traceparent: parent=%s trace=%s", sel.Parent().SpanID(), sel.SpanContext().TraceID())
	}
	a := attrs(sel)
	if got := a["db.query.text"].AsString(); got != "select * from t where id = ?" {
		t.Errorf("query text = %q", got)
	}
	if a["db.collection.name"].AsString() != "t" || a["db.namespace"].AsString() != "postgres" || a["db.operation.name"].AsString() != "SELECT" {
		t.Errorf("bad identity attributes: %v", a)
	}
	// plan and statement lines were merged into ONE span, with the statement's duration
	if d := sel.EndTime().Sub(sel.StartTime()); d != 894*time.Microsecond {
		t.Errorf("duration = %v, want 894µs (from the statement line, not the plan's 19µs)", d)
	}
	if want := time.Date(2026, 10, 28, 18, 42, 12, 0, time.UTC); !sel.EndTime().Equal(want) {
		t.Errorf("end time = %v", sel.EndTime())
	}
	if a["db.response.returned_rows"].AsInt64() != 1 || a["postgresql.buffers.shared.hit"].AsInt64() != 2 {
		t.Errorf("plan-derived attributes missing: %v", a)
	}
	if !strings.Contains(a["postgresql.query.plan"].AsString(), "Index Scan using t_pkey") {
		t.Errorf("plan missing")
	}

	if errSpan.Status().Code.String() != "Error" || errSpan.Parent().SpanID().String() != "00f067aa0ba902b7" {
		t.Errorf("error span: status=%v parent=%s", errSpan.Status(), errSpan.Parent().SpanID())
	}
	if errSpan.Status().Description != "" {
		t.Errorf("error message must not be exported unless query-text=full, got %q", errSpan.Status().Description)
	}
}

func TestInterleavedBackendsAreKeptApart(t *testing.T) {
	tp2 := strings.ReplaceAll(tp1, "00f067aa0ba902b7", "1111111111111111")
	log := "2026-10-28 10:00:00.100 UTC [1] u@d LOG:  duration: 0.010 ms  plan:\n\tQuery Text: select 1 " + tp1 + "\n\tResult  (cost=0.00..0.01 rows=1 width=4)\n" +
		"2026-10-28 10:00:00.101 UTC [2] u@d LOG:  duration: 0.020 ms  plan:\n\tQuery Text: select 2 " + tp2 + "\n\tResult  (cost=0.00..0.01 rows=1 width=4)\n" +
		"2026-10-28 10:00:00.102 UTC [2] u@d LOG:  duration: 2.000 ms  statement: select 2 " + tp2 + "\n" +
		"2026-10-28 10:00:00.103 UTC [1] u@d LOG:  duration: 1.000 ms  statement: select 1 " + tp1 + "\n"
	spans := run(t, "%m [%p] %u@%d ", log, defaultCfg)
	if len(spans) != 2 {
		t.Fatalf("want 2 spans, got %d", len(spans))
	}
	for _, s := range spans {
		q := attrs(s)["db.query.text"].AsString()
		want := map[string]string{"select ?": ""}
		_ = want
		d := s.EndTime().Sub(s.StartTime())
		switch s.Parent().SpanID().String() {
		case "00f067aa0ba902b7":
			if d != time.Millisecond {
				t.Errorf("backend 1 got duration %v (%s)", d, q)
			}
		case "1111111111111111":
			if d != 2*time.Millisecond {
				t.Errorf("backend 2 got duration %v (%s)", d, q)
			}
		default:
			t.Errorf("unexpected parent %s", s.Parent().SpanID())
		}
	}
}

func TestExtendedProtocolIgnoresParameters(t *testing.T) {
	log := "2026-10-28 10:00:00.100 UTC [5] app@shop LOG:  duration: 0.300 ms  execute <unnamed>: select v from t where email = $1 " + tp1 + "\n" +
		"2026-10-28 10:00:00.100 UTC [5] app@shop DETAIL:  parameters: $1 = 'alice@example.com'\n"
	spans := run(t, "%m [%p] %u@%d ", log, Config{QueryText: "sanitized"})
	if len(spans) != 1 {
		t.Fatalf("want 1 span, got %d", len(spans))
	}
	for _, kv := range spans[0].Attributes() {
		if strings.Contains(kv.Value.Emit(), "alice@example.com") {
			t.Fatalf("bound parameter leaked into %s", kv.Key)
		}
	}
	if got := attrs(spans[0])["db.query.text"].AsString(); got != "select v from t where email = $1" {
		t.Errorf("query text = %q", got)
	}
}

func TestSamplingAndUntracedRules(t *testing.T) {
	unsampled := strings.ReplaceAll(tp1, "-01'", "-00'")
	log := "2026-10-28 10:00:00.100 UTC [5] u@d LOG:  duration: 1.000 ms  statement: select 1 " + unsampled + "\n" +
		"2026-10-28 10:00:00.200 UTC [5] u@d LOG:  duration: 1.000 ms  statement: select 2\n" +
		"2026-10-28 10:00:00.300 UTC [5] u@d LOG:  duration: 1.000 ms  statement: select 'traceparent=''00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01'''\n"
	if n := len(run(t, "%m [%p] %u@%d ", log, Config{QueryText: "sanitized"})); n != 0 {
		t.Errorf("default: want 0 spans (unsampled, untraced, traceparent-in-a-string-literal), got %d", n)
	}
	spans := run(t, "%m [%p] %u@%d ", log, Config{QueryText: "sanitized", TraceUntraced: true})
	if len(spans) != 2 { // the unsampled parent is still honoured; the other two become roots
		t.Fatalf("trace-untraced: want 2 root spans, got %d", len(spans))
	}
	for _, s := range spans {
		if s.Parent().IsValid() {
			t.Errorf("root span has a parent")
		}
	}
}

func TestPlanLiteralsRedactedUnlessFull(t *testing.T) {
	log := "2026-10-28 10:00:00.100 UTC [5] u@d LOG:  duration: 1.000 ms  plan:\n" +
		"\tQuery Text: select * from t where email = 'hunter2@example.com' " + tp1 + "\n" +
		"\tSeq Scan on t  (cost=0.00..1.00 rows=1 width=4)\n" +
		"\t  Filter: (email = 'hunter2@example.com'::text)\n"
	spans := run(t, "%m [%p] %u@%d ", log, defaultCfg)
	if len(spans) != 1 {
		t.Fatal("want 1 span")
	}
	for k, v := range attrs(spans[0]) {
		if strings.Contains(v.Emit(), "hunter2") {
			t.Errorf("literal leaked into %s", k)
		}
	}
	full := run(t, "%m [%p] %u@%d ", log, Config{QueryText: "full", IncludePlans: true})
	if !strings.Contains(attrs(full[0])["postgresql.query.plan"].AsString(), "hunter2") {
		t.Errorf("full mode should keep the plan verbatim")
	}
	noPlan := run(t, "%m [%p] %u@%d ", log, Config{QueryText: "sanitized"})
	if _, ok := attrs(noPlan[0])["postgresql.query.plan"]; ok {
		t.Errorf("plans must be opt-in")
	}
}

func TestStockPrefixWithQAndNonSessionLines(t *testing.T) {
	// checkpointer lines have no user/db part (%q); they must not be glued onto the previous record
	log := "2026-10-28 10:00:00.100 UTC [5] u@d 00000 LOG:  duration: 1.000 ms  statement: select 1 " + tp1 + "\n" +
		"2026-10-28 10:00:01.000 UTC [30] LOG:  checkpoint starting: time\n" +
		"2026-10-28 10:00:01.100 UTC [5] u@d 23505 ERROR:  duplicate key value violates unique constraint \"t_pkey\"\n" +
		"2026-10-28 10:00:01.100 UTC [5] u@d 23505 DETAIL:  Key (id)=(1) already exists.\n" +
		"2026-10-28 10:00:01.100 UTC [5] u@d 23505 STATEMENT:  insert into t values (1) " + tp1 + "\n"
	spans := run(t, "%m [%p] %q%u@%d %e ", log, Config{QueryText: "full"})
	if len(spans) != 2 {
		t.Fatalf("want 2 spans, got %d", len(spans))
	}
	e := attrs(spans[1])
	if e["error.type"].AsString() != "23505" || spans[1].Status().Description != "duplicate key value violates unique constraint \"t_pkey\"" {
		t.Errorf("error attrs: %v / %q", e, spans[1].Status().Description)
	}
	if strings.Contains(spans[1].Status().Description, "already exists") {
		t.Errorf("DETAIL (contains data values) must not be exported")
	}
}

func TestMultiLineQueryTextInPlan(t *testing.T) {
	log := "2026-10-28 10:00:00.100 UTC [5] u@d LOG:  duration: 1.000 ms  plan:\n" +
		"\tQuery Text: select *\n\t  from orders\n\t where id = 7 " + tp1 + "\n" +
		"\tSeq Scan on orders  (cost=0.00..1.00 rows=1 width=4)\n"
	spans := run(t, "%m [%p] %u@%d ", log, defaultCfg)
	if len(spans) != 1 || attrs(spans[0])["db.query.text"].AsString() != "select * from orders where id = ?" {
		t.Fatalf("got %v", spans)
	}
}

func TestNumericTimezoneOffset(t *testing.T) {
	p, _ := CompilePrefix("%m [%p] ")
	r, ok := p.Parse("2026-10-28 12:00:00.250 +01 [9] LOG:  hello")
	if !ok || !r.Time.Equal(time.Date(2026, 10, 28, 11, 0, 0, 250e6, time.UTC)) {
		t.Fatalf("got %v %v", r.Time, ok)
	}
}

func TestRedactMatchesExtensionCases(t *testing.T) {
	// same cases as the extension's integration test, so both modes emit identical db.query.text
	cases := [][2]string{
		{"select 'it''s' as a1, $$x$$ as c, $t$ y $t$ as d, 1.5e-3 as e, .5 as f, 7 as \"col 1\", E'a\\'b' as g from pg_class limit 3",
			"select ? as a1, ? as c, ? as d, ? as e, ? as f, ? as \"col 1\", ? as g from pg_class limit ?"},
		{"select  1 /* a /* nested */ b */ , -- tail\n 2\n  from pg_class", "select ? , ? from pg_class"},
		{"select relname from pg_class where relname = 'x'  and relnatts > 2 ", "select relname from pg_class where relname = ? and relnatts > ?"},
		{"select * from t1 where c2 = $1 and name = 'unterminated", "select * from t1 where c2 = $1 and name = ?"},
	}
	for _, c := range cases {
		if got := Redact(c[0], true); got != c[1] {
			t.Errorf("Redact(%q)\n got %q\nwant %q", c[0], got, c[1])
		}
	}
}

func TestCollectionHeuristic(t *testing.T) {
	cases := map[string]string{
		"select * from orders where id = ?":              "orders",
		"insert into public.orders (a) values (?)":       "orders",
		"update orders set a = ?":                        "orders",
		"delete from orders where id = ?":                "orders",
		"select * from a join b on a.id = b.id":          "",
		"select * from a join a x on x.id = a.id":        "a",
		"select * from generate_series(?, ?)":            "",
		"select extract(year from d) from orders":        "orders",
		"select * from t where a is distinct from b":     "t",
		"select * from t where id in (select id from u)": "",
	}
	for q, want := range cases {
		if got := collectionFromSQL(q); got != want {
			t.Errorf("collectionFromSQL(%q) = %q, want %q", q, got, want)
		}
	}
}

func TestBuffersParsing(t *testing.T) {
	p, ok := ParseAutoExplain("\nQuery Text: select 1\nSeq Scan on t  (cost=0.00..1.00 rows=1 width=4) (actual rows=3 loops=1)\n  Buffers: shared hit=2 read=1 dirtied=4, temp read=5 written=6\n")
	if !ok || p.Buffers["shared.hit"] != 2 || p.Buffers["shared.read"] != 1 || p.Buffers["shared.dirtied"] != 4 ||
		p.Buffers["temp.read"] != 5 || p.Buffers["temp.written"] != 6 || p.Rows != 3 {
		t.Fatalf("%+v ok=%v", p, ok)
	}
}

func TestPrefixWithoutItsTrailingSpace(t *testing.T) {
	// "log_line_prefix = '%m [%p] %q%u@%d %e '" passed through YAML/shell loses the final space
	log := "2026-10-28 10:00:00.100 UTC [5] u@d 00000 LOG:  duration: 1.000 ms  statement: select 1 " + tp1 + "\n"
	if n := len(run(t, "%m [%p] %q%u@%d %e", log, Config{QueryText: "sanitized"})); n != 1 {
		t.Fatalf("want 1 span, got %d", n)
	}
}
