# Standalone SBSE Controller

A containerized Go port of the `sbse_controller` ESP32 firmware module
(`software/src/modules/sbse_controller/`). It controls an **SMA Sunny Boy Smart
Energy (SBSE) hybrid inverter** over Modbus TCP — the same regime-based P+I+D
control law, the same REST/MQTT/dashboard surface, and the same SMA-compatible
Modbus server/proxy that lets evcc / a WARP charger steer it — but runs as a Docker
container or Kubernetes pod instead of on an ESP32.

The firmware module remains the **behavioral source of truth**. This is a
reimplementation, not a transpile, so the control law and SBSE register map are
copied verbatim and locked down with unit tests built from the worked examples in
[`../../software/src/modules/sbse_controller/CONFIG.md`](../../software/src/modules/sbse_controller/CONFIG.md).
For the full config/state/command reference, read that `CONFIG.md` — the field
names, ranges, semantics and Modbus register contract are identical here.

## What it does

1. **Modbus client** to the SBSE: reads grid power (unit 2), battery charge/discharge
   and SoC (unit 3) every `tick_ms`, writes the battery active-power setpoint.
2. **Control law**: keeps the grid inside a `[grid_charge_target_w,
   grid_discharge_target_w]` deadzone (regime-based, direction-locked), with SoC and
   saturation clamps, output smoothing, the safety-zero net and the inverter
   keep-alive heartbeat.
3. **REST + MQTT + dashboard**: `state`, `config`, `active_config`, `pause`/`resume`
   and a 5-minute live-trace chart, identical semantics across all three transports.
4. **SMA-compatible Modbus server + proxy**: accepts the WriteMultipleRegisters
   traffic a "SMA Hybrid Inverter" client sends, and answers evcc's `sma-hybrid`
   reads from a synthesized/polled register cache.

## Architecture

```
cmd/sbse-controller/main.go   wiring, tickers, signal handling (0 W on shutdown)
internal/config               Config + ActiveConfig, validators, JSON + env overrides
internal/inverter             mutex-guarded Modbus client + SBSE register map
internal/control              control law (law.go) + state machine (controller/loop)
internal/trace                5-min 1 Hz ring buffer (/history)
internal/smaserver            SMA Modbus server (simonvetter) + read-side proxy cache
internal/api                  REST handlers, SSE hub, MQTT mirror
web/                          Preact + uPlot dashboard (embedded via go:embed)
```

The inverter connection is shared by the control loop and the proxy poller behind a
single mutex (mirroring the firmware pool's serialization); all controller state is
guarded by one mutex and the mutex is never held across a blocking Modbus
transaction.

## Build

```sh
# Dashboard (regenerates web/dist, which is committed for go:embed):
cd web && npm install && npm run build && cd ..

# Binary:
go build -o sbse-controller ./cmd/sbse-controller

# Tests (control-law convergence vs CONFIG.md, proxy, Modbus round-trip):
go test ./...

# Container:
docker build -t sbse-controller:latest .
```

## Run

### Docker Compose

Edit `deploy/docker-compose.yml` (set `SBSE_HOST` to your inverter IP), then:

```sh
docker compose -f deploy/docker-compose.yml up -d
```

### Kubernetes

Edit the ConfigMap in `deploy/k8s.yaml` (inverter IP, targets), then:

```sh
kubectl apply -f deploy/k8s.yaml
```

> ⚠️ **Singleton.** This commands real battery hardware. The Deployment is
> `replicas: 1` with `strategy: Recreate` — never scale it up; two controllers would
> fight over the inverter.

### Bare binary

```sh
SBSE_ENABLED=true SBSE_HOST=192.168.110.151 ./sbse-controller
```

## Configuration

Config is loaded from `$SBSE_CONFIG_PATH` (default `/data/config.json`, replacing the
firmware's NVS), then overlaid with `SBSE_*` environment variables, then live-tunable
via the REST/MQTT `*_update` endpoints. `active_config` is RAM-only and reseeded from
`config` on boot — exactly as in the firmware.

**Deployment env** (no firmware analogue): `SBSE_HTTP_ADDR` (default `:8080`),
`SBSE_CONFIG_PATH`, `SBSE_MQTT_ENABLED`, `SBSE_MQTT_BROKER`, `SBSE_MQTT_PREFIX`,
`SBSE_MQTT_USERNAME`, `SBSE_MQTT_PASSWORD`.

**Controller env** mirrors every `config` field as `SBSE_<UPPER_SNAKE>` — e.g.
`SBSE_HOST`, `SBSE_PORT`, `SBSE_TICK_MS`, `SBSE_GRID_CHARGE_TARGET_W`,
`SBSE_GRID_DISCHARGE_TARGET_W`, `SBSE_MAX_CHARGE_W`, `SBSE_MODBUS_SERVER_ENABLED`,
`SBSE_MODBUS_SERVER_PORT`, `SBSE_MODBUS_SERVER_UNIT_ID`,
`SBSE_MODBUS_SERVER_AUTHORITY`, etc.

### REST API (mirrors `CONFIG.md`)

```sh
curl http://localhost:8080/sbse_controller/state
curl http://localhost:8080/sbse_controller/active_config
# Live-update one field (merge: send only what you change):
curl -X PUT http://localhost:8080/sbse_controller/active_config \
     -H 'Content-Type: application/json' -d '{"grid_discharge_target_w":200}'
curl -X POST http://localhost:8080/sbse_controller/pause
curl http://localhost:8080/sbse_controller/history | jq '.samples | length'
```

The dashboard is at `http://localhost:8080/` (live chart seeded from `/history`, kept
live via SSE on `/sbse_controller/state/sse`).

## Differences from the firmware

- **Persistence**: JSON file on a volume instead of NVS.
- **Connection recovery**: the firmware relied on the Modbus pool's socket-level
  disconnect detection; here a read-failure streak forces a reconnect (see
  `reconnectThresholdLocked`).
- **No** Wireiguard/Wifi/OTA — the container uses the host/pod network.
- **Port 502** is privileged. The image runs as root so the SMA server can bind it;
  for a hardened deploy set `SBSE_MODBUS_SERVER_PORT > 1024` (the k8s manifest also
  shows granting `NET_BIND_SERVICE` instead).
- REST PUTs **merge** onto the current object (send only changed fields) rather than
  requiring the full object.

## Verifying against hardware

Point it at the real SBSE on your LAN and compare its `/history` trace to the
firmware's for an hour. Drive the SMA server with the `pymodbus` snippets in
`CONFIG.md` (OpMod force-charge, partial `40793` writes, authority levels) and/or a
real evcc `sma-hybrid` poll.
