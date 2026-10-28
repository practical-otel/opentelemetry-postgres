CREATE EXTENSION opentelemetry_postgres;

CREATE TABLE orders (id serial PRIMARY KEY, customer text NOT NULL, total numeric NOT NULL);
INSERT INTO orders (customer, total) VALUES ('alice@example.com', 42.50), ('bob@example.com', 13.00);
