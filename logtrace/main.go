// pg-log-tracer turns PostgreSQL logs into OpenTelemetry traces.
//
// It is the no-extension counterpart of the opentelemetry-postgres extension, for servers
// where you cannot load C extensions (Amazon RDS/Aurora, Cloud SQL, Azure, ...). The query
// text in Postgres's own logs carries the sqlcommenter traceparent, so each logged statement
// becomes a span parented to the application's client span.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	sdkresource "go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/trace"
)

var version = "0.1.0" // overridden at release time with -ldflags "-X main.version=..."

func main() {
	var (
		file      = flag.String("file", "", "Postgres log file to follow (tail -F). Reads stdin if no input is given")
		fromStart = flag.Bool("from-start", false, "with --file: read existing content instead of only new lines")
		listen    = flag.String("listen", "", "also accept OTLP/HTTP logs on this address (e.g. :4319), path /v1/logs")
		prefix    = flag.String("log-line-prefix", "%m [%p] ", "the server's log_line_prefix; must contain %p and %m or %t")
		service   = flag.String("service-name", envOr("OTEL_SERVICE_NAME", "postgresql"), "service.name of the emitted spans")
		endpoint  = flag.String("endpoint", "", "OTLP/HTTP traces endpoint (default: OTEL_EXPORTER_OTLP_* variables, then http://localhost:4318)")
		queryText = flag.String("query-text", "sanitized", "db.query.text: off | sanitized | full")
		plans     = flag.Bool("include-plans", false, "attach auto_explain plans (numeric constants in plans are NOT redacted)")
		untraced  = flag.Bool("trace-untraced", false, "also emit root spans for statements without a traceparent")
		minDur    = flag.Duration("min-duration", 0, "skip statements faster than this (errors are always kept)")
		showVer   = flag.Bool("version", false, "print version")
	)
	flag.Parse()
	if *showVer {
		fmt.Println("pg-log-tracer", version)
		return
	}
	if *queryText != "off" && *queryText != "sanitized" && *queryText != "full" {
		log.Fatalf("--query-text must be off, sanitized or full")
	}

	pfx, err := CompilePrefix(*prefix)
	if err != nil {
		log.Fatalf("--log-line-prefix: %v", err)
	}
	pfx.warnTZFn = func(tz string) {
		log.Printf("warning: log timestamps use time zone %q; only UTC, GMT and numeric offsets are understood, assuming UTC. Set log_timezone = 'UTC' on the server", tz)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	opts := []otlptracehttp.Option{}
	if *endpoint != "" {
		opts = append(opts, otlptracehttp.WithEndpointURL(*endpoint))
	}
	exp, err := otlptracehttp.New(ctx, opts...)
	if err != nil {
		log.Fatalf("OTLP exporter: %v", err)
	}
	res, _ := sdkresource.Merge(sdkresource.Default(), sdkresource.NewSchemaless(attribute.String("service.name", *service)))
	tp := sdktrace.NewTracerProvider(sdktrace.WithBatcher(exp, sdktrace.WithBatchTimeout(time.Second)), sdktrace.WithResource(res))

	builder := &Builder{tracer: tp.Tracer("pg-log-tracer", trace.WithInstrumentationVersion(version)), cfg: Config{
		QueryText: *queryText, IncludePlans: *plans, TraceUntraced: *untraced, MinDuration: *minDur}}
	asm := NewAssembler(builder.Handle)
	rd := NewReader(pfx, asm.Handle)

	lines := make(chan string, 4096)
	inputs := 0
	if *file != "" {
		inputs++
		go followFile(ctx, *file, *fromStart, lines)
	}
	if *listen != "" {
		inputs++
		go serveOTLP(ctx, *listen, lines)
	}
	if inputs == 0 {
		go func() { readStream(os.Stdin, lines); close(lines) }()
	}

	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	statsTick := time.NewTicker(time.Minute)
	defer statsTick.Stop()
	stats := func() string {
		return fmt.Sprintf("lines=%d records=%d statements=%d %s", rd.Lines, rd.Records, asm.Statements, builder.C)
	}
	lastStats := ""
	lastLine := time.Now()
loop:
	for {
		select {
		case l, ok := <-lines:
			if !ok {
				break loop
			}
			rd.Feed(l)
			lastLine = time.Now()
		case <-tick.C:
			if time.Since(lastLine) > time.Second { // input idle: the record in progress is complete
				rd.Flush()
			}
			asm.Expire(time.Now())
		case <-statsTick.C:
			if s := stats(); s != lastStats {
				lastStats = s
				log.Print(s)
				if rd.Lines > 0 && rd.Records == 0 {
					log.Printf("warning: %d lines read but none matched --log-line-prefix %q; check it equals the server's log_line_prefix", rd.Lines, *prefix)
				}
			}
		case <-ctx.Done():
			break loop
		}
	}
	rd.Flush()
	asm.FlushAll()
	shutdown, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = tp.Shutdown(shutdown)
	log.Printf("done: %s", stats())
}

func envOr(k, def string) string {
	if v := strings.TrimSpace(os.Getenv(k)); v != "" {
		return v
	}
	return def
}
