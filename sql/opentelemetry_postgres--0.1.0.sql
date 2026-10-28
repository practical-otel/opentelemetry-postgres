-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION opentelemetry_postgres" to load this file. \quit

-- Counters for the exporter. Tracing itself needs only shared_preload_libraries;
-- this extension exists to expose these.
CREATE FUNCTION otel_stats(
    OUT enqueued bigint,         -- spans accepted into the ring since server start
    OUT dropped bigint,          -- spans discarded because the ring was full
    OUT exported bigint,         -- spans delivered to the collector
    OUT export_failures bigint,  -- failed export requests
    OUT export_dropped bigint,   -- spans lost in failed export requests
    OUT queue_depth bigint       -- spans waiting for export right now
) RETURNS record
AS 'MODULE_PATHNAME', 'otel_stats'
LANGUAGE C STRICT;
