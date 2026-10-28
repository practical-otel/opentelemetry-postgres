package main

import (
	"fmt"
	"regexp"
	"strings"
	"time"
)

// Record is one Postgres log entry: a header line plus any continuation lines.
type Record struct {
	Time     time.Time
	PID      int
	User     string
	DB       string
	App      string
	SQLState string
	Severity string // LOG, ERROR, STATEMENT, DETAIL, ...
	Message  string // may contain newlines (continuation lines, tab stripped)
}

// Prefix parses the part of a log line that log_line_prefix produces.
type Prefix struct {
	re        *regexp.Regexp
	hasTime   bool
	timeIsEp  bool
	warnedTZ  bool
	warnTZFn  func(string)
	timeNames []string
}

// escape -> regex fragment (named group where we care about the value)
var prefixEscapes = map[byte]string{
	'a': `(?P<app>.*?)`,
	'u': `(?P<user>.*?)`,
	'd': `(?P<db>.*?)`,
	'r': `.*?`,
	'h': `.*?`,
	'p': `(?P<pid>\d+)`,
	't': `(?P<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(?: [A-Za-z0-9+\-]+)?)`,
	'm': `(?P<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+(?: [A-Za-z0-9+\-]+)?)`,
	'n': `(?P<epoch>\d+\.\d+)`,
	'e': `(?P<sqlstate>[0-9A-Z]{5})`,
	'c': `[0-9a-f.]+`,
	'l': `\d+`,
	's': `\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(?: [A-Za-z0-9+\-]+)?`,
	'i': `.*?`,
	'v': `[\d/]*`,
	'x': `\d+`,
}

// CompilePrefix turns a log_line_prefix such as "%m [%p] %q%u@%d " into a line parser.
// Everything after %q only appears for sessions, so it is optional.
func CompilePrefix(prefix string) (*Prefix, error) {
	var a, b strings.Builder
	cur := &a
	seen := map[string]bool{}
	for i := 0; i < len(prefix); i++ {
		c := prefix[i]
		if c != '%' {
			cur.WriteString(regexp.QuoteMeta(string(c)))
			continue
		}
		i++
		if i >= len(prefix) {
			return nil, fmt.Errorf("log_line_prefix ends with a lone %%")
		}
		e := prefix[i]
		switch {
		case e == '%':
			cur.WriteString("%")
		case e == 'q':
			cur = &b
		default:
			frag, ok := prefixEscapes[e]
			if !ok {
				return nil, fmt.Errorf("unsupported log_line_prefix escape %%%c", e)
			}
			// a named group may only appear once in a Go regexp
			for _, name := range []string{"app", "user", "db", "pid", "ts", "epoch", "sqlstate"} {
				if strings.Contains(frag, "(?P<"+name+">") {
					if seen[name] {
						frag = `.*?`
					}
					seen[name] = true
				}
			}
			cur.WriteString(frag)
		}
	}
	if !seen["pid"] {
		return nil, fmt.Errorf("log_line_prefix must contain %%p (process id) so interleaved sessions can be told apart")
	}
	if !seen["ts"] && !seen["epoch"] {
		return nil, fmt.Errorf("log_line_prefix must contain %%m or %%t (timestamp); span times come from it")
	}
	expr := `^` + a.String()
	if b.Len() > 0 {
		expr += `(?:` + b.String() + `)?`
	}
	// \s*: a trailing space in log_line_prefix is easily lost (shells, YAML), so don't insist on it
	expr += `\s*(?P<sev>[A-Z]+[0-9]?):  (?P<msg>.*)$`
	re, err := regexp.Compile(expr)
	if err != nil {
		return nil, err
	}
	return &Prefix{re: re}, nil
}

var tzOffset = regexp.MustCompile(`^([+-])(\d\d)(\d\d)?$`)

// parseTime understands %t/%m output. Postgres prints a zone abbreviation; only
// UTC/GMT and numeric offsets are unambiguous, anything else is treated as UTC (with a warning).
func (p *Prefix) parseTime(s string) (time.Time, error) {
	base, tz := s, ""
	if i := strings.IndexByte(s, ' '); i >= 0 {
		if j := strings.IndexByte(s[i+1:], ' '); j >= 0 {
			base, tz = s[:i+1+j], s[i+2+j:]
		}
	}
	layout := "2006-01-02 15:04:05"
	if strings.Contains(base, ".") {
		layout = "2006-01-02 15:04:05.999999999"
	}
	t, err := time.ParseInLocation(layout, base, time.UTC)
	if err != nil {
		return t, err
	}
	switch {
	case tz == "" || tz == "UTC" || tz == "GMT" || tz == "Z":
	case tzOffset.MatchString(tz):
		m := tzOffset.FindStringSubmatch(tz)
		off := atoi(m[2])*3600 + atoi(m[3])*60
		if m[1] == "-" {
			off = -off
		}
		t = t.Add(-time.Duration(off) * time.Second)
	default:
		if p.warnTZFn != nil && !p.warnedTZ {
			p.warnedTZ = true
			p.warnTZFn(tz)
		}
	}
	return t, nil
}

func atoi(s string) int {
	n := 0
	for _, c := range s {
		n = n*10 + int(c-'0')
	}
	return n
}

// Parse returns ok=false when the line is not the start of a log record.
func (p *Prefix) Parse(line string) (Record, bool) {
	m := p.re.FindStringSubmatch(line)
	if m == nil {
		return Record{}, false
	}
	r := Record{}
	for i, name := range p.re.SubexpNames() {
		v := m[i]
		switch name {
		case "app":
			r.App = v
		case "user":
			r.User = v
		case "db":
			r.DB = v
		case "pid":
			r.PID = atoi(v)
		case "sqlstate":
			if v != "00000" {
				r.SQLState = v
			}
		case "sev":
			r.Severity = v
		case "msg":
			r.Message = v
		case "ts":
			if t, err := p.parseTime(v); err == nil {
				r.Time = t
			}
		case "epoch":
			var sec, frac int64
			fmt.Sscanf(v, "%d.%d", &sec, &frac)
			r.Time = time.Unix(sec, frac*1e6).UTC()
		}
	}
	if r.Time.IsZero() {
		r.Time = time.Now().UTC()
	}
	return r, true
}
