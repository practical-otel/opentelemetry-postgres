package main

import (
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Statement is one finished query reconstructed from the log.
type Statement struct {
	End         time.Time
	DurationMs  float64
	PID         int
	DB, User    string
	Query       string
	Plan        *Plan
	ErrSQLState string
	ErrMessage  string
	IsError     bool
}

type pendingPlan struct {
	rec  Record
	dur  float64
	plan Plan
}

type pendingErr struct {
	rec Record
}

// Assembler turns the interleaved record stream into Statements. Postgres writes the
// pieces of one query as separate lines, in this order per backend process:
//
//	LOG:  duration: 0.02 ms  plan:      (auto_explain; optional)
//	LOG:  duration: 0.90 ms  statement: SELECT ...   (or "execute <name>: ...")
//
// and, for a failed query:
//
//	ERROR:  division by zero
//	STATEMENT:  SELECT ...
type Assembler struct {
	plans  map[int]*pendingPlan
	errs   map[int]*pendingErr
	emit   func(Statement)
	maxAge time.Duration
	seen   map[int]time.Time // wall-clock time we last saw each pending entry

	Statements int
}

func NewAssembler(emit func(Statement)) *Assembler {
	a := &Assembler{plans: map[int]*pendingPlan{}, errs: map[int]*pendingErr{},
		maxAge: 2 * time.Second, seen: map[int]time.Time{}}
	a.emit = func(s Statement) { a.Statements++; emit(s) }
	return a
}

var reDuration = regexp.MustCompile(`(?s)^duration: ([0-9.]+) ms\s+(.*)$`)
var reExecute = regexp.MustCompile(`(?s)^execute [^:]*: (.*)$`)

func (a *Assembler) Handle(r Record) {
	switch r.Severity {
	case "DETAIL", "HINT", "CONTEXT", "QUERY", "LOCATION":
		return // parameters and error context: never forwarded
	case "STATEMENT":
		if pe := a.errs[r.PID]; pe != nil {
			delete(a.errs, r.PID)
			a.emit(Statement{End: pe.rec.Time, PID: r.PID, DB: pe.rec.DB, User: pe.rec.User,
				Query: r.Message, IsError: true, ErrSQLState: pe.rec.SQLState, ErrMessage: pe.rec.Message})
		}
		return
	}

	// any other record from this backend ends what was pending for it
	delete(a.errs, r.PID)

	if r.Severity == "ERROR" || r.Severity == "FATAL" {
		a.flushPlan(r.PID)
		a.errs[r.PID] = &pendingErr{rec: r}
		a.seen[r.PID] = time.Now()
		return
	}
	if r.Severity != "LOG" {
		return
	}
	m := reDuration.FindStringSubmatch(r.Message)
	if m == nil {
		return
	}
	dur, _ := strconv.ParseFloat(m[1], 64)
	rest := m[2]

	switch {
	case strings.HasPrefix(rest, "plan:"):
		a.flushPlan(r.PID)
		if p, ok := ParseAutoExplain(strings.TrimPrefix(rest, "plan:")); ok {
			a.plans[r.PID] = &pendingPlan{rec: r, dur: dur, plan: p}
			a.seen[r.PID] = time.Now()
		}
	case strings.HasPrefix(rest, "statement: "), reExecute.MatchString(rest):
		query := strings.TrimPrefix(rest, "statement: ")
		if e := reExecute.FindStringSubmatch(rest); e != nil {
			query = e[1]
		}
		st := Statement{End: r.Time, DurationMs: dur, PID: r.PID, DB: r.DB, User: r.User, Query: query}
		if pp := a.plans[r.PID]; pp != nil {
			delete(a.plans, r.PID)
			if sameQuery(pp.plan.Query, query) {
				st.Plan = &pp.plan
			} else {
				a.emitPlan(pp)
			}
		}
		a.emit(st)
	}
}

func sameQuery(planQuery, stmt string) bool {
	planQuery, stmt = strings.TrimSpace(planQuery), strings.TrimSpace(stmt)
	return planQuery == stmt || strings.Contains(stmt, planQuery)
}

func (a *Assembler) flushPlan(pid int) {
	if pp := a.plans[pid]; pp != nil {
		delete(a.plans, pid)
		a.emitPlan(pp)
	}
}

func (a *Assembler) emitPlan(pp *pendingPlan) {
	a.emit(Statement{End: pp.rec.Time, DurationMs: pp.dur, PID: pp.rec.PID, DB: pp.rec.DB,
		User: pp.rec.User, Query: pp.plan.Query, Plan: &pp.plan})
}

// Expire flushes entries that waited longer than maxAge for their other half, so a
// plan without a duration line (auto_explain only) still becomes a span promptly.
func (a *Assembler) Expire(now time.Time) {
	for pid, t := range a.seen {
		if now.Sub(t) < a.maxAge {
			continue
		}
		a.flushPlan(pid)
		delete(a.errs, pid)
		delete(a.seen, pid)
	}
}

func (a *Assembler) FlushAll() {
	for pid := range a.plans {
		a.flushPlan(pid)
	}
	a.errs = map[int]*pendingErr{}
	a.seen = map[int]time.Time{}
}
