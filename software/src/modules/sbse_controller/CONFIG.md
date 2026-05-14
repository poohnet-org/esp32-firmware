# `sbse_controller` configuration reference

The controller exposes two configuration endpoints. They share most fields but
have different persistence and update semantics.

| Endpoint | Persisted to NVS | Hot-reload (no reboot) | Typical use |
|---|:---:|:---:|---|
| `sbse_controller/config`        | yes | only for the live-tunable subset; init-only fields need a reboot | Boot defaults / policy changes |
| `sbse_controller/active_config` | no  | yes                                                              | Frequent / automated steering; flash-friendly |

`active_config` is re-seeded from `config` on every boot.

JSON payloads must contain **all** keys of the target object (`force_same_keys`
is on for HTTP PUTs and MQTT topic writes). The standard pattern is
GET → modify → PUT.

## Init-only fields (persistent `config` only)

| Field | Default | Units / range | Purpose |
|---|---:|---|---|
| `enabled` | `false` | bool | Master switch. When `false`, the controller never connects and `tick()` short-circuits to `mode: disabled`. Requires a reboot to take effect because the network-connect event that starts the TCP connection has already been delivered. |
| `host` | `""` | string (≤ 64 chars) | SBSE Modbus TCP IP / hostname. The validator rejects `enabled: true` with an empty host. |
| `port` | `502` | uint16 | Modbus TCP port on the SBSE. Standard is 502. |
| `tick_ms` | `300` | ms, 50…5000 | Control loop period. One read-compute-write cycle per tick. Lower = faster grid tracking but more Modbus traffic. |
| `soc_interval_ms` | `1000` | ms, ≥ `tick_ms`, ≤ 60000 | How often to poll battery SoC. SoC changes slowly; polling every tick wastes bandwidth. |

## Live-tunable fields (in both `config` and `active_config`)

| Field | Default | Units / range | Purpose |
|---|---:|---|---|
| `target_grid_w` | `0` | W, −10 000…+10 000 | **Grid exchange setpoint.** Positive = import from grid, negative = export. `0` is pure self-consumption (zero net exchange). `+200` forces 200 W of grid import (battery charges to make up the gap). `−200` forces 200 W of feed-in (battery discharges). |
| `max_charge_w` | `5000` | W, 0…10 000 | Upper bound on battery charging power. The computed setpoint is hard-clamped to `[−max_charge_w, +max_discharge_w]`. `0` disables charging. |
| `max_discharge_w` | `5000` | W, 0…10 000 | Upper bound on battery discharging power. `0` disables discharging. |
| `kp_milli` | `1000` (= Kp 1.0) | Kp × 1000, 100…2000 | **Proportional controller gain.** Per tick: `new_setpoint = battery_now + Kp · (ema_grid − target_grid_w) + Kd · Δema_grid`. `Kp = 1.0` ≈ "one-shot correction." Lower (~0.5) = slower, more damped. Higher (~1.5) = snappier but can ring against the EMA filter. |
| `kd_milli` | `0` (= Kd 0.0, disabled) | Kd × 1000, 0…3000 | **Derivative gain on the smoothed grid measurement** (`Δema_grid = ema_grid − previous_ema_grid` per tick). Derivative-on-measurement avoids a kick when `target_grid_w` changes. Anticipates fast load steps that the EMA would otherwise see only after one or two cycles of lag. Set to `0` for the classic P + implicit-I controller. Raise toward `1.0`–`2.0` if induction plates / kettles cause overshoot. Too high → ringing. |
| `alpha_grid_milli` | `300` (= α 0.30) | α × 1000, 10…1000 | **EMA on the grid-power input.** `ema = α · measured + (1−α) · ema`. `α = 0.30` averages roughly the last 3–5 samples. `α → 1.0` disables smoothing; `α → 0` is heavy damping with lag. |
| `alpha_setpoint_milli` | `700` (= α 0.70) | α × 1000, 10…1000 | **EMA on the commanded battery setpoint** (post-controller, pre-write). Smooths the output to the inverter so noisy grid readings don't produce flickery commands. |
| `deadband_w` | `50` | W, 0…1000 | **Write-suppression threshold.** If the new computed setpoint is within ±`deadband_w` of the last commanded one, the Modbus write is skipped (inverter holds the last value). Cuts cell churn and bus traffic at idle. SBSE has no internal-control fallback, so generous deadbands are safe. |
| `safety_zero_after_failures` | `5` | count, 0…100 | **Safety net.** After this many consecutive failed read cycles, the controller commands a one-shot 0 W setpoint and enters `mode: safety` until reads recover. At default 300 ms tick × 5 → ~1.5 s trip latency. `0` disables the safety net (last setpoint held forever during outages). |
| `simulation_mode` | `false` | bool | **Simulation mode.** When `true`, the controller runs every cycle exactly as in real operation -- reads, EMAs, P-controller, clamps, deadband -- but skips the actual Modbus write to the inverter. `last_setpoint_w`, `write_ok_count`, the chart and the deadband logic all behave as if the writes had gone out, so you can verify tuning without commanding the battery. The dashboard shows a `SIM` badge and `state.simulation_mode` is exposed for monitoring. |

## Read-only state (`sbse_controller/state`)

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `disabled` / `not_connected` / `stale` / `running` / `paused` / `safety` / `faulted`. |
| `last_setpoint_w` | int32 | Last value successfully written to the inverter. |
| `last_write_age_ms` | uint32 | Milliseconds since the last successful write. Grows when the deadband suppresses writes. |
| `grid_w_raw` | int32 | Most recently read grid power, unsmoothed. |
| `grid_w_ema` | int32 | Smoothed grid power (input to the controller). |
| `battery_w` | int32 | Most recently read battery power. Positive = discharging, negative = charging. |
| `battery_soc` | uint8 | Battery SoC, 0…100; `255` until first read succeeds. |
| `write_ok_count` | uint32 | Lifetime counter of successful setpoint writes. |
| `write_err_count` | uint32 | Lifetime counter of failed setpoint writes. |
| `read_fail_streak` | uint32 | Current run of consecutive failed read cycles; resets on the next successful cycle. |
| `simulation_mode` | bool | Mirrors `active_config.simulation_mode`. When `true`, the controller is computing setpoints but **not writing them to the inverter**. |
| `last_error` | string | Last error message produced by a read or write. |

## Commands

| Command | Payload | Effect |
|---|---|---|
| `sbse_controller/config_update` | full `config` object | Validates, writes NVS, hot-reloads live-tunable subset. Equivalent to `PUT /sbse_controller/config`. |
| `sbse_controller/active_config_update` | full `active_config` object | Validates, updates runtime fields. No flash write. Equivalent to `PUT /sbse_controller/active_config`. |
| `sbse_controller/force_release` | empty | Writes one 0 W setpoint and pauses the loop for 30 s. `mode → paused`. |

## Controller loop in one diagram

```
each tick (default 300 ms, gated by `enabled` + connection + `paused`):

  measured_grid   = read GRID_POWER_ADDR (unit 2)
  battery_now     = read BATTERY_POWER_ADDR (unit 3)        ── if either fails, skip cycle
  soc             = read BATTERY_SOC_ADDR (unit 3, if due)     and increment read_fail_streak

  ema_grid       ← α_grid · measured_grid  + (1 − α_grid) · ema_grid
  d_ema_grid     = ema_grid − previous_ema_grid    (0 on the first cycle)
  delta          = ema_grid − target_grid_w
  raw_setpoint   = battery_now + Kp · delta + Kd · d_ema_grid
  raw_setpoint   = clamp(raw_setpoint, −max_charge_w, +max_discharge_w)
  raw_setpoint   = clamp(raw_setpoint, by SoC: 100 % blocks charge, 0 % blocks discharge)
  ema_setpoint   ← α_set · raw_setpoint + (1 − α_set) · ema_setpoint
  target_w       = round(ema_setpoint)

  if |target_w − last_written_w| < deadband_w → skip write (inverter holds last setpoint)
  else                                          → write target_w to POWER_SETPOINT_ADDR (unit 3)

  if read_fail_streak == safety_zero_after_failures → write 0 W, mode = safety
```

## Tuning cookbook

| Symptom | Knob | Direction |
|---|---|---|
| Grid setpoint oscillates / overshoots | `kp_milli`, `alpha_setpoint_milli` | Lower Kp first; then raise `alpha_setpoint_milli` (less smoothing) only if response feels lagged. |
| Fast load steps (induction plate, kettle) cause overshoot | `kd_milli` | Raise from `0` toward `1000`–`2000` (Kd 1.0–2.0). The D-on-measurement term anticipates rapid grid changes that the EMA otherwise sees too late. Too high → ringing. |
| Battery cycles too much at idle | `deadband_w`, `alpha_setpoint_milli` | Increase deadband (100–200 W); lower `alpha_setpoint_milli` for more output damping. |
| Response feels sluggish | `alpha_grid_milli`, `kp_milli` | Raise `alpha_grid_milli` toward 1000 (less input smoothing); modest Kp bump (~1.2). |
| Cap how hard the battery can work | `max_charge_w` / `max_discharge_w` | Lower the relevant cap; `0` disables that direction entirely. |
| Pure self-consumption | `target_grid_w` | `0` (default). |
| Dump excess to grid | `target_grid_w` | Negative value (e.g. `−200`). |
| Charge from grid (low tariff) | `target_grid_w` | Positive value; only effective while `battery_soc < 100`. |
| Battery keeps its last setpoint during a Modbus outage | `safety_zero_after_failures` | Set to `0` (disables the safety-zero trip). |

## Useful curl examples

```bash
# Read current effective values:
curl http://<sbse-controller-ip>/sbse_controller/active_config

# Live update one field (GET → modify → PUT):
curl http://<sbse-controller-ip>/sbse_controller/active_config \
  | jq '.target_grid_w = 200' \
  | curl -X PUT http://<sbse-controller-ip>/sbse_controller/active_config \
         -H 'Content-Type: application/json' -d @-

# Change the persistent boot default (writes flash, also hot-reloads):
curl http://<sbse-controller-ip>/sbse_controller/config \
  | jq '.deadband_w = 100' \
  | curl -X PUT http://<sbse-controller-ip>/sbse_controller/config \
         -H 'Content-Type: application/json' -d @-

# Force the loop to release control for 30 s:
curl -X POST http://<sbse-controller-ip>/sbse_controller/force_release

# Watch the controller:
watch -n1 'curl -s http://<sbse-controller-ip>/sbse_controller/state | jq'
```
