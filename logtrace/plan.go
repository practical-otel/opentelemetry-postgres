package main

import (
	"encoding/json"
	"regexp"
	"strconv"
	"strings"
)

// Plan is what we can learn from an auto_explain log entry.
type Plan struct {
	Query    string
	Text     string // plan as logged (text format) or the raw JSON
	Format   string // "text" | "json"
	Analyzed bool
	Rows     int64 // top node "actual rows", when analyzed
	HasRows  bool
	Buffers  map[string]int64 // shared hit/read/dirtied/written, temp read/written (top node)
	Tables   []string         // relations scanned/modified
}

var (
	reCostLine = regexp.MustCompile(`\(cost=[\d.]+\.\.[\d.]+ rows=\d+ width=\d+\)`)
	reActual   = regexp.MustCompile(`\(actual (?:time=[\d.]+\.\.[\d.]+ )?rows=(\d+)(?:\.\d+)? loops=\d+\)`)
	reBuffers  = regexp.MustCompile(`^\s*Buffers:\s*(.*)$`)
	reNodeOn   = regexp.MustCompile(`(?:Scan|Insert|Update|Delete|Merge)(?: using \S+)? on (?:ONLY )?(\S+)`)
	reKV       = regexp.MustCompile(`(shared|local|temp)? ?(hit|read|dirtied|written)=(\d+)`)
)

// ParseAutoExplain parses the body after "duration: X ms  plan:" (text or JSON format).
func ParseAutoExplain(body string) (Plan, bool) {
	body = strings.TrimLeft(body, "\n")
	if strings.HasPrefix(strings.TrimSpace(body), "{") {
		var j struct {
			QueryText string          `json:"Query Text"`
			Plan      json.RawMessage `json:"Plan"`
		}
		if json.Unmarshal([]byte(body), &j) != nil || j.QueryText == "" {
			return Plan{}, false
		}
		return Plan{Query: j.QueryText, Text: strings.TrimSpace(body), Format: "json",
			Analyzed: strings.Contains(string(j.Plan), `"Actual Rows"`)}, true
	}
	lines := strings.Split(body, "\n")
	if len(lines) == 0 || !strings.HasPrefix(lines[0], "Query Text: ") {
		return Plan{}, false
	}
	// The query text may span lines; the plan starts at the first line with "(cost=".
	start := -1
	for i, l := range lines {
		if reCostLine.MatchString(l) {
			start = i
			break
		}
	}
	if start < 0 {
		return Plan{}, false
	}
	p := Plan{
		Query:  strings.TrimPrefix(strings.Join(lines[:start], "\n"), "Query Text: "),
		Text:   strings.Join(lines[start:], "\n"),
		Format: "text",
	}
	p.Analyzed = reActual.MatchString(lines[start])
	if m := reActual.FindStringSubmatch(lines[start]); m != nil {
		p.Rows, _ = strconv.ParseInt(m[1], 10, 64)
		p.HasRows = true
	}
	seen := map[string]bool{}
	for i := start; i < len(lines); i++ {
		if m := reNodeOn.FindStringSubmatch(lines[i]); m != nil && !seen[m[1]] {
			seen[m[1]] = true
			p.Tables = append(p.Tables, m[1])
		}
		// the first Buffers: line after the top node belongs to the top node
		if p.Buffers == nil && i > start {
			if m := reBuffers.FindStringSubmatch(lines[i]); m != nil {
				p.Buffers = map[string]int64{}
				for _, kv := range reKV.FindAllStringSubmatch(m[1], -1) {
					v, _ := strconv.ParseInt(kv[3], 10, 64)
					scope := kv[1]
					if scope == "" { // "read=1 written=2" continues the previous scope
						scope = lastScope(m[1], kv[0])
					}
					p.Buffers[scope+"."+kv[2]] += v
				}
			}
		}
	}
	return p, true
}

// lastScope finds the scope word (shared/local/temp) that governs a bare "read=" / "written=".
func lastScope(all, kv string) string {
	idx := strings.Index(all, kv)
	scope := "shared"
	for _, s := range []string{"shared", "local", "temp"} {
		if j := strings.LastIndex(all[:idx], s); j >= 0 && j > strings.LastIndex(all[:idx], scope) {
			scope = s
		}
	}
	return scope
}
