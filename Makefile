EXTENSION    = opentelemetry_postgres
DATA         = sql/opentelemetry_postgres--0.1.0.sql
MODULE_big   = opentelemetry_postgres
OBJS         = src/otel_init.o src/otel_hooks.o src/otel_sanitize.o \
               src/otel_otlp.o src/otel_http.o src/otel_worker.o
PG_CPPFLAGS  = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

PG_CONFIG   ?= pg_config
PGXS        := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# End-to-end tests (run `make install` first): starts a throwaway cluster and a
# fake OTLP collector. Needs uv (https://docs.astral.sh/uv/).
.PHONY: integration
integration:
	PG_CONFIG=$(PG_CONFIG) uv run test/integration.py
