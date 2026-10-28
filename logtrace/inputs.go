package main

import (
	"bufio"
	"compress/gzip"
	"context"
	"io"
	"log"
	"net/http"
	"os"
	"time"

	collogs "go.opentelemetry.io/proto/otlp/collector/logs/v1"
	"google.golang.org/protobuf/proto"
)

// readStream sends every line of r to out.
func readStream(r io.Reader, out chan<- string) {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<20), 16<<20) // plans and long queries make long lines
	for sc.Scan() {
		out <- sc.Text()
	}
}

// followFile tails path like `tail -F`: starts at the end (or the start with fromStart),
// and reopens the file after rotation or truncation.
func followFile(ctx context.Context, path string, fromStart bool, out chan<- string) {
	var f *os.File
	var br *bufio.Reader
	var pos int64
	open := func(seekEnd bool) {
		var err error
		if f != nil {
			f.Close()
		}
		f, err = os.Open(path)
		if err != nil {
			f = nil
			return
		}
		if seekEnd {
			pos, _ = f.Seek(0, io.SeekEnd)
		} else {
			pos = 0
		}
		br = bufio.NewReaderSize(f, 1<<20)
	}
	open(!fromStart)
	var partial string
	for ctx.Err() == nil {
		if f == nil {
			time.Sleep(200 * time.Millisecond)
			open(false)
			continue
		}
		line, err := br.ReadString('\n')
		pos += int64(len(line))
		if err == nil {
			out <- partial + line[:len(line)-1]
			partial = ""
			continue
		}
		partial += line
		time.Sleep(200 * time.Millisecond)
		// rotated or truncated?
		fi, ferr := os.Stat(path)
		cur, _ := f.Stat()
		if ferr != nil || (cur != nil && !os.SameFile(fi, cur)) || (fi != nil && fi.Size() < pos) {
			open(false)
			partial = ""
		}
	}
}

// otlpLogsHandler accepts OTLP/HTTP logs (protobuf, optionally gzip). Every log record body
// is treated as text from a Postgres log, so any Collector receiver can feed this
// (filelog, awscloudwatch, googlecloudpubsub, azureeventhub, ...).
func otlpLogsHandler(out chan<- string) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/logs", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		if ct := r.Header.Get("Content-Type"); ct != "application/x-protobuf" {
			http.Error(w, "send OTLP logs as application/x-protobuf (otlphttp exporter encoding: proto)", http.StatusUnsupportedMediaType)
			return
		}
		var body io.Reader = r.Body
		if r.Header.Get("Content-Encoding") == "gzip" {
			gz, err := gzip.NewReader(r.Body)
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			defer gz.Close()
			body = gz
		}
		raw, err := io.ReadAll(io.LimitReader(body, 64<<20))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		var req collogs.ExportLogsServiceRequest
		if err := proto.Unmarshal(raw, &req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		for _, rl := range req.ResourceLogs {
			for _, sl := range rl.ScopeLogs {
				for _, lr := range sl.LogRecords {
					if s := lr.Body.GetStringValue(); s != "" {
						out <- s
					}
				}
			}
		}
		resp, _ := proto.Marshal(&collogs.ExportLogsServiceResponse{})
		w.Header().Set("Content-Type", "application/x-protobuf")
		w.Write(resp)
	})
	return mux
}

func serveOTLP(ctx context.Context, addr string, out chan<- string) {
	srv := &http.Server{Addr: addr, Handler: otlpLogsHandler(out), ReadHeaderTimeout: 10 * time.Second}
	go func() { <-ctx.Done(); srv.Close() }()
	log.Printf("listening for OTLP/HTTP logs on %s/v1/logs", addr)
	if err := srv.ListenAndServe(); err != nil && ctx.Err() == nil {
		log.Fatalf("OTLP logs listener: %v", err)
	}
}
