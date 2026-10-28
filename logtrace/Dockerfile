FROM golang:1.27 AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /pg-log-tracer .

FROM gcr.io/distroless/static-debian12
COPY --from=build /pg-log-tracer /pg-log-tracer
ENTRYPOINT ["/pg-log-tracer"]
