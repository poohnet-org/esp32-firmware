# syntax=docker/dockerfile:1

# --- Stage 1: build the dashboard --------------------------------------------
FROM node:22-alpine AS web
WORKDIR /web
COPY web/package.json web/package-lock.json* ./
RUN npm install --no-audit --no-fund
COPY web/ ./
RUN npm run build

# --- Stage 2: build the Go binary --------------------------------------------
FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
# Bring in the freshly built dashboard so go:embed picks it up.
COPY --from=web /web/dist ./web/dist
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o /sbse-controller ./cmd/sbse-controller

# --- Stage 3: minimal runtime image ------------------------------------------
# Root variant so the SMA server can bind the privileged default port 502. For a
# hardened deployment, set modbus_server_port > 1024 (or grant NET_BIND_SERVICE)
# and run as non-root -- see deploy/deployment.yaml.
FROM gcr.io/distroless/static-debian12
COPY --from=build /sbse-controller /sbse-controller
# HTTP dashboard/API + SMA-compatible Modbus server.
EXPOSE 8080 502
VOLUME ["/data"]
ENTRYPOINT ["/sbse-controller"]
