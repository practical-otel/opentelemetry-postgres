package main

import (
	"encoding/hex"
	"regexp"
)

type TraceParent struct {
	TraceID [16]byte
	SpanID  [8]byte
	Sampled bool
}

var (
	reComment     = regexp.MustCompile(`(?s)/\*(.*?)\*/`)
	reTraceparent = regexp.MustCompile(`traceparent='([0-9a-f]{2})-([0-9a-f]{32})-([0-9a-f]{16})-([0-9a-f]{2})'`)
)

// FindTraceParent extracts a sqlcommenter traceparent from inside a /* */ comment,
// with the same validity rules as the extension (lowercase hex, version != ff, non-zero ids).
func FindTraceParent(query string) (tp TraceParent, found bool) {
	for _, c := range reComment.FindAllStringSubmatch(query, -1) {
		m := reTraceparent.FindStringSubmatch(c[1])
		if m == nil || m[1] == "ff" {
			continue
		}
		var t TraceParent
		tid, _ := hex.DecodeString(m[2])
		sid, _ := hex.DecodeString(m[3])
		fl, _ := hex.DecodeString(m[4])
		copy(t.TraceID[:], tid)
		copy(t.SpanID[:], sid)
		if t.TraceID == ([16]byte{}) || t.SpanID == ([8]byte{}) {
			continue
		}
		t.Sampled = fl[0]&1 == 1
		return t, true
	}
	return TraceParent{}, false
}
