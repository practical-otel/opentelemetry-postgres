"""A small web app whose SQL carries sqlcommenter traceparents.

The OpenTelemetry psycopg2 instrumentation with enable_commenter=True appends
/*traceparent='00-<trace>-<client span>-01'*/ to every statement. opentelemetry-postgres
reads it, so the span Postgres records is a child of the instrumentation's CLIENT span.
"""
import os

import psycopg2
from flask import Flask, jsonify, request
from opentelemetry import trace
from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
from opentelemetry.instrumentation.flask import FlaskInstrumentor
from opentelemetry.instrumentation.psycopg2 import Psycopg2Instrumentor
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor

# service.name, endpoint and batch delay come from the standard OTEL_* variables.
provider = TracerProvider()
provider.add_span_processor(BatchSpanProcessor(OTLPSpanExporter()))
trace.set_tracer_provider(provider)

Psycopg2Instrumentor().instrument(
    enable_commenter=True,
    commenter_options={"opentelemetry_values": True},
)

app = Flask(__name__)
FlaskInstrumentor().instrument_app(app)


def db():
    conn = psycopg2.connect(os.environ["DATABASE_URL"])
    conn.autocommit = True
    return conn


@app.get("/healthz")
def healthz():
    return "ok"


@app.get("/orders/<int:order_id>")
def get_order(order_id):
    with db() as conn, conn.cursor() as cur:
        cur.execute("SELECT id, customer, total FROM orders WHERE id = %s", (order_id,))
        row = cur.fetchone()
    if row is None:
        return jsonify(error="not found"), 404
    return jsonify(id=row[0], customer=row[1], total=float(row[2]))


@app.post("/orders")
def create_order():
    body = request.get_json(force=True)
    with db() as conn, conn.cursor() as cur:
        cur.execute(
            "INSERT INTO orders (customer, total) VALUES (%s, %s) RETURNING id",
            (body["customer"], body["total"]),
        )
        return jsonify(id=cur.fetchone()[0]), 201


@app.get("/fail")
def fail():
    with db() as conn, conn.cursor() as cur:
        cur.execute("SELECT total / (total - total) FROM orders")  # division by zero at run time
    return "unreachable"


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, threaded=True)
