package main

import (
	"regexp"
	"strings"
)

func isIdentChar(c byte) bool {
	return c == '_' || c == '$' || c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}
func isDigit(c byte) bool { return c >= '0' && c <= '9' }
func isAlnum(c byte) bool { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') }
func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'
}

// Redact replaces literals with '?'. It is the same algorithm as the C extension's
// otel_redact, so both modes produce identical db.query.text.
//
// sqlMode=true  (query text): strips comments (which removes the sqlcommenter payload),
//
//	redacts string, dollar-quoted and numeric literals, collapses whitespace.
//
// sqlMode=false (plans): redacts string literals only.
func Redact(s string, sqlMode bool) string {
	var out strings.Builder
	n := len(s)
	putSpace := func() {
		if out.Len() > 0 && out.String()[out.Len()-1] != ' ' {
			out.WriteByte(' ')
		}
	}
	for i := 0; i < n; {
		c := s[i]
		prev := byte(' ')
		if i > 0 {
			prev = s[i-1]
		}

		if c == '\'' { // 'string' and E'string'
			esc := (prev == 'E' || prev == 'e') && (i < 2 || !isIdentChar(s[i-2]))
			j := i + 1
			for j < n {
				if esc && s[j] == '\\' {
					j += 2
					continue
				}
				if s[j] == '\'' {
					if j+1 < n && s[j+1] == '\'' {
						j += 2
						continue
					}
					break
				}
				j++
			}
			if esc {
				if cur := out.String(); len(cur) > 0 {
					out.Reset()
					out.WriteString(cur[:len(cur)-1]) // drop the E prefix
				}
			}
			out.WriteByte('?')
			i = j + 1
			continue
		}

		if c == '"' { // "quoted identifier": verbatim
			j := i + 1
			for j < n {
				if s[j] == '"' {
					if j+1 < n && s[j+1] == '"' {
						j += 2
						continue
					}
					break
				}
				j++
			}
			end := j + 1
			if end > n {
				end = n
			}
			out.WriteString(s[i:end])
			i = j + 1
			continue
		}

		if c == '$' && !isIdentChar(prev) { // $1 or $tag$ ... $tag$
			j := i + 1
			if j < n && isDigit(s[j]) {
				for j < n && isDigit(s[j]) {
					j++
				}
				out.WriteString(s[i:j])
				i = j
				continue
			}
			for j < n && (isAlnum(s[j]) || s[j] == '_' || s[j] >= 0x80) && !(j == i+1 && isDigit(s[j])) {
				j++
			}
			if j < n && s[j] == '$' {
				tag := s[i : j+1]
				if k := strings.Index(s[j+1:], tag); k >= 0 {
					i = j + 1 + k + len(tag)
				} else {
					i = n
				}
				out.WriteByte('?')
				continue
			}
		}

		if sqlMode {
			if c == '/' && i+1 < n && s[i+1] == '*' { // block comment, nestable
				depth := 1
				i += 2
				for i < n && depth > 0 {
					switch {
					case s[i] == '/' && i+1 < n && s[i+1] == '*':
						depth++
						i += 2
					case s[i] == '*' && i+1 < n && s[i+1] == '/':
						depth--
						i += 2
					default:
						i++
					}
				}
				putSpace()
				continue
			}
			if c == '-' && i+1 < n && s[i+1] == '-' { // line comment
				for i < n && s[i] != '\n' {
					i++
				}
				putSpace()
				continue
			}
			if (isDigit(c) || (c == '.' && i+1 < n && isDigit(s[i+1]))) && !isIdentChar(prev) && prev != '.' {
				j := i + 1
				hex := c == '0' && j < n && (s[j] == 'x' || s[j] == 'X')
				for j < n && (isAlnum(s[j]) || s[j] == '_' || s[j] == '.' ||
					(!hex && (s[j] == '+' || s[j] == '-') && (s[j-1] == 'e' || s[j-1] == 'E'))) {
					j++
				}
				out.WriteByte('?')
				i = j
				continue
			}
			if isSpace(c) {
				putSpace()
				i++
				continue
			}
		}
		out.WriteByte(c)
		i++
	}
	res := out.String()
	if sqlMode {
		res = strings.TrimRight(res, " ")
	}
	return res
}

var (
	reTableRef = regexp.MustCompile(`(?i)\b(from|join|into|update)\s+(?:only\s+)?((?:"[^"]+"|[\w$]+)(?:\.(?:"[^"]+"|[\w$]+))?)(\s*\()?`)
	// FROM that is not a table reference
	reNotTable = regexp.MustCompile(`(?i)\b(?:is\s+(?:not\s+)?distinct|(?:extract|substring|trim|overlay)\s*\([^)]*?)\s+from\b`)
	reSubquery = regexp.MustCompile(`(?i)\(\s*(?:select|with|values)\b`)
)

// collectionFromSQL guesses the single table a statement touches from its redacted text.
// It returns "" whenever that is not clear-cut (joins of different tables, subqueries, ...).
func collectionFromSQL(redacted string) string {
	if reSubquery.MatchString(redacted) {
		return ""
	}
	cleaned := reNotTable.ReplaceAllString(redacted, " ")
	names := map[string]bool{}
	var last string
	for _, m := range reTableRef.FindAllStringSubmatch(cleaned, -1) {
		kw := strings.ToLower(m[1])
		if m[3] != "" && (kw == "from" || kw == "join") { // function call, e.g. generate_series(
			continue
		}
		name := m[2]
		if i := strings.LastIndexByte(name, '.'); i >= 0 {
			name = name[i+1:]
		}
		name = strings.Trim(name, `"`)
		names[name] = true
		last = name
	}
	if len(names) == 1 {
		return last
	}
	return ""
}

var reFirstWord = regexp.MustCompile(`^\s*([A-Za-z]+)`)

func operationName(redacted string) string {
	m := reFirstWord.FindStringSubmatch(redacted)
	if m == nil {
		return "QUERY"
	}
	return strings.ToUpper(m[1])
}
