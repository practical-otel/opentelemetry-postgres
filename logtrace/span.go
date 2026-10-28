package main

import (
	"context"
	"fmt"
	"strings"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/trace"
)

type Config struct {
	QueryText     string // off | sanitized | full
	IncludePlans  bool
	TraceUntraced bool
	MinDuration   time.Duration
}

type Counters struct{ Spans, SkippedNoParent, SkippedUnsampled, SkippedFast int }

type Builder struct {
	tracer trace.Tracer
	cfg    Config
	C      Counters
}

func (b *Builder) Handle(st Statement) {
	tp, found := FindTraceParent(st.Query)
	if found && !tp.Sampled {
		b.C.SkippedUnsampled++
		return
	}
	if !found && !b.cfg.TraceUntraced {
		b.C.SkippedNoParent++
		return
	}
	if !st.IsError && time.Duration(st.DurationMs*float64(time.Millisecond)) < b.cfg.MinDuration {
		b.C.SkippedFast++
		return
	}

	redacted := Redact(st.Query, true)
	op := operationName(redacted)
	coll := ""
	if st.Plan != nil && len(st.Plan.Tables) == 1 {
		coll = st.Plan.Tables[0]
		if i := strings.LastIndexByte(coll, '.'); i >= 0 {
			coll = coll[i+1:]
		}
		coll = strings.Trim(coll, `"`)
	} else if st.Plan == nil || len(st.Plan.Tables) == 0 {
		coll = collectionFromSQL(redacted)
	}

	end := st.End
	start := end.Add(-time.Duration(st.DurationMs * float64(time.Millisecond)))

	attrs := []attribute.KeyValue{
		attribute.String("db.system.name", "postgresql"),
		attribute.String("db.operation.name", op),
		attribute.Int("postgresql.backend.pid", st.PID),
		attribute.String("postgresql.span.source", "log"),
	}
	if st.DB != "" {
		attrs = append(attrs, attribute.String("db.namespace", st.DB))
	}
	summary := op
	name := op + " " + st.DB
	if coll != "" {
		attrs = append(attrs, attribute.String("db.collection.name", coll))
		summary = op + " " + coll
		name = summary
	}
	attrs = append(attrs, attribute.String("db.query.summary", summary))

	switch b.cfg.QueryText {
	case "full":
		attrs = append(attrs, attribute.String("db.query.text", st.Query))
	case "sanitized":
		attrs = append(attrs, attribute.String("db.query.text", redacted))
	}

	if p := st.Plan; p != nil {
		if p.HasRows && op == "SELECT" {
			attrs = append(attrs, attribute.Int64("db.response.returned_rows", p.Rows))
		}
		for _, k := range []string{"shared.hit", "shared.read", "shared.dirtied", "shared.written", "temp.read", "temp.written"} {
			if v := p.Buffers[k]; v != 0 {
				attrs = append(attrs, attribute.Int64("postgresql.buffers."+k, v))
			}
		}
		if b.cfg.IncludePlans {
			text := p.Text
			if b.cfg.QueryText != "full" {
				text = Redact(text, false)
			}
			attrs = append(attrs,
				attribute.String("postgresql.query.plan", text),
				attribute.String("postgresql.query.plan.format", p.Format),
				attribute.Bool("postgresql.query.plan.analyzed", p.Analyzed))
		}
	}

	ctx := context.Background()
	if found {
		ctx = trace.ContextWithRemoteSpanContext(ctx, trace.NewSpanContext(trace.SpanContextConfig{
			TraceID: trace.TraceID(tp.TraceID), SpanID: trace.SpanID(tp.SpanID),
			TraceFlags: trace.FlagsSampled, Remote: true,
		}))
	}
	_, span := b.tracer.Start(ctx, name, trace.WithTimestamp(start), trace.WithSpanKind(trace.SpanKindServer),
		trace.WithAttributes(attrs...))
	if st.IsError {
		code := st.ErrSQLState
		if code != "" {
			span.SetAttributes(attribute.String("error.type", code), attribute.String("db.response.status_code", code))
		}
		msg := ""
		if b.cfg.QueryText == "full" {
			msg = st.ErrMessage
		}
		span.SetStatus(codes.Error, msg)
	}
	span.End(trace.WithTimestamp(end))
	b.C.Spans++
}

func (c Counters) String() string {
	return fmt.Sprintf("spans=%d skipped(no traceparent)=%d skipped(unsampled)=%d skipped(fast)=%d",
		c.Spans, c.SkippedNoParent, c.SkippedUnsampled, c.SkippedFast)
}
