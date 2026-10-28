# PostgreSQL with opentelemetry-postgres preinstalled.
#
#   docker build -t opentelemetry-postgres --build-arg PG_MAJOR=17 .
#   docker run -e POSTGRES_PASSWORD=pw \
#     -e OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4318 opentelemetry-postgres
#
# Build with `--target test` to run the end-to-end suite against that Postgres major.
ARG PG_MAJOR=17

FROM postgres:${PG_MAJOR} AS build
ARG PG_MAJOR
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential postgresql-server-dev-${PG_MAJOR} \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile opentelemetry_postgres.control ./
COPY sql sql
COPY src src
RUN make && make install DESTDIR=/out && make install

FROM build AS test
COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv
COPY test test
RUN chown -R postgres /src
USER postgres
RUN make integration

FROM postgres:${PG_MAJOR}
COPY --from=build /out/ /
# Preload the library by default; override with your own -c flags as needed.
CMD ["postgres", "-c", "shared_preload_libraries=opentelemetry_postgres"]
