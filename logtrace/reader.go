package main

import "strings"

// Reader groups raw log lines into Records. A line that matches the prefix starts a new
// record; anything else (plans, multi-line queries) continues the previous one.
type Reader struct {
	prefix *Prefix
	cur    *Record
	emit   func(Record)

	Lines, Records, Unmatched int // Unmatched: lines before the first record header (wrong --log-line-prefix?)
}

func NewReader(p *Prefix, emit func(Record)) *Reader { return &Reader{prefix: p, emit: emit} }

// Feed accepts text containing any number of lines (a single line, or a whole multi-line
// log event as delivered by some log pipelines).
func (r *Reader) Feed(text string) {
	for _, line := range strings.Split(strings.TrimRight(text, "\r\n"), "\n") {
		line = strings.TrimRight(line, "\r")
		r.Lines++
		if rec, ok := r.prefix.Parse(line); ok {
			r.Records++
			r.Flush()
			r.cur = &rec
			continue
		}
		if r.cur != nil {
			r.cur.Message += "\n" + strings.TrimPrefix(line, "\t")
		} else {
			r.Unmatched++
		}
	}
}

// Flush emits the record being assembled. Call it when the input goes idle or ends.
func (r *Reader) Flush() {
	if r.cur != nil {
		rec := *r.cur
		r.cur = nil
		r.emit(rec)
	}
}
