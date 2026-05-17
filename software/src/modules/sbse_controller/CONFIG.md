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

### Transport equivalence

Every endpoint listed below is reachable over **all** of the following with
identical semantics:

- **HTTP** at `http://<sbse-controller-ip>/sbse_controller/<endpoint>`
  (`GET` for state, `PUT` for config, `POST` for commands).
- **MQTT** under the configured topic prefix at `<prefix>/sbse_controller/<endpoint>`.
  States and configs are published on every change; configs and commands
  accept writes on the corresponding `…/update` and command topics.
- **Built-in dashboard** at `http://<sbse-controller-ip>/` — Status page
  (live trace + active_config sliders + commands) and SBSE Controller
  sub-page (persistent config).

## Init-only fields (persistent `config` only)

| Field | Default | Units / range | Purpose |
|---|---:|---|---|
| `enabled` | `false` | bool | Master switch. When `false`, the controller never connects and `tick()` short-circuits to `mode: disabled`. Requires a reboot to take effect because the network-connect event that starts the TCP connection has already been delivered. |
| `host` | `""` | string (≤ 64 chars) | SBSE Modbus TCP IP / hostname. The validator rejects `enabled: true` with an empty host. |
| `port` | `502` | uint16 | Modbus TCP port on the SBSE. Standard is 502. |
| `tick_ms` | `300` | ms, 50…5000 | Control loop period. One read-compute-write cycle per tick. Lower = faster grid tracking but more Modbus traffic. |
| `soc_interval_ms` | `1000` | ms, ≥ `tick_ms`, ≤ 60000 | How often to poll battery SoC. SoC changes slowly; polling every tick wastes bandwidth. |
| `modbus_server_enabled` | `false` | bool | Enable the SMA-compatible Modbus TCP server (see [Modbus TCP server](#modbus-tcp-server-external-control) below). Requires a reboot to take effect. |
| `modbus_server_port` | `502` | uint16 | Port the SMA-compatible server listens on. Requires a reboot to take effect. |
| `modbus_server_unit_id` | `3` | uint8, 0…247 | Unit ID this server responds to. `0` means "accept any". Live-tunable (no reboot needed). |
| `modbus_server_watchdog_s` | `60` | seconds, 0…3600 | Watchdog timeout. If no Modbus write arrives within this many seconds, the controller reverts the live overrides to the persistent config and exits force-mode. `0` disables the watchdog. Live-tunable. |
| `modbus_server_use_grid_spt` | `false` | bool | When `false` (default), Modbus writes to register 40793 leave `target_grid_w` alone -- only `max_charge_w` and `max_discharge_w` are updated. This matches the WARP charger's behaviour (it always sends `GridWSpt = 0` regardless of mode). Set to `true` for Modbus clients that genuinely steer the grid setpoint. Live-tunable. |

## Live-tunable fields (in both `config` and `active_config`)

| Field | Default | Units / range | Purpose |
|---|---:|---|---|
| `target_grid_w` | `0` | W, −10 000…+10 000 | **Grid exchange setpoint** (a wish). Positive = import from grid, negative = export. `0` is pure self-consumption. The controller tries to reach this value; if the battery can't physically deliver it (SoC limit, max-charge/discharge saturation, no PV available), the controller saturates at the closest feasible state -- the grid then balances wherever it ends up. There is **no cross-field constraint** with `max_charge_w` / `max_discharge_w`: setting `target_grid_w = +1000 W` with `max_charge_w = 100 W` is perfectly valid and means "charge as hard as you can, up to 100 W, even if the grid then still imports 900 W to cover the rest." |
| `max_charge_w` | `5000` | W, 0…10 000 | **Battery saturation limit, charge direction.** Hard cap on charging power. The computed setpoint is clamped to `[−max_charge_w, +max_discharge_w]` before being written. `0` disables charging entirely. Independent of `target_grid_w`. |
| `max_discharge_w` | `5000` | W, 0…10 000 | **Battery saturation limit, discharge direction.** Hard cap on discharging power. `0` disables discharging entirely. Independent of `target_grid_w`. |
| `kp_milli` | `1000` (= Kp 1.0) | Kp × 1000, 100…2000 | **Proportional controller gain.** Per tick: `new_setpoint = battery_now + Kp · (ema_grid − target_grid_w) + Kd · Δema_grid`. `Kp = 1.0` ≈ "one-shot correction." Lower (~0.5) = slower, more damped. Higher (~1.5) = snappier but can ring against the EMA filter. |
| `kd_milli` | `0` (= Kd 0.0, disabled) | Kd × 1000, 0…3000 | **Derivative gain on the smoothed grid measurement** (`Δema_grid = ema_grid − previous_ema_grid` per tick). Derivative-on-measurement avoids a kick when `target_grid_w` changes. Anticipates fast load steps that the EMA would otherwise see only after one or two cycles of lag. Set to `0` for the classic P + implicit-I controller. Raise toward `1.0`–`2.0` if induction plates / kettles cause overshoot. Too high → ringing. |
| `alpha_grid_milli` | `300` (= α 0.30) | α × 1000, 10…1000 | **EMA on the grid-power input.** `ema = α · measured + (1−α) · ema`. `α = 0.30` averages roughly the last 3–5 samples. `α → 1.0` disables smoothing; `α → 0` is heavy damping with lag. |
| `alpha_setpoint_milli` | `700` (= α 0.70) | α × 1000, 10…1000 | **EMA on the commanded battery setpoint** (post-controller, pre-write). Smooths the output to the inverter so noisy grid readings don't produce flickery commands. |
| `deadband_w` | `50` | W, 0…1000 | **Write-suppression threshold.** If the new computed setpoint is within ±`deadband_w` of the last commanded one, the Modbus write is skipped (inverter holds the last value). Cuts cell churn and bus traffic at idle. SBSE has no internal-control fallback, so generous deadbands are safe. |
| `safety_zero_after_failures` | `5` | count, 0…100 | **Safety net.** After this many consecutive failed read cycles, the controller commands a one-shot 0 W setpoint and enters `mode: safety` until reads recover. At default 300 ms tick × 5 → ~1.5 s trip latency. `0` disables the safety net (last setpoint held forever during outages). |
| `simulation_mode` | `false` | bool | **Simulation mode.** When `true`, the controller runs every cycle exactly as in real operation -- reads, EMAs, P-controller, clamps, deadband -- but skips the actual Modbus write to the inverter. `last_setpoint_w`, `write_ok_count`, the chart and the deadband logic all behave as if the writes had gone out, so you can verify tuning without commanding the battery. The dashboard shows a `SIM` badge and `state.simulation_mode` is exposed for monitoring. |
| `soft_target` | `false` | bool | **Hard / soft target semantics.** When `false` (default), `target_grid_w` is a *hard* setpoint and the controller will discharge the battery if needed to chase it in both directions. When `true`, see [Soft target semantics](#soft-target-semantics) below. The dashboard shows a `HARD` / `SOFT` badge accordingly. |

## Read-only state (`sbse_controller/state`)

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `disabled` / `not_connected` / `stale` / `running` / `paused` / `safety` / `faulted` / `force_charge` / `force_discharge` / `blocked` (both maxes 0) / `block_charge` (`max_charge_w == 0`) / `block_discharge` (`max_discharge_w == 0`). The block-* values are derived from the saturation limits regardless of who set them (Modbus, dashboard, MQTT). |
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
| `soft_target` | bool | Mirrors `active_config.soft_target`. See [Soft target semantics](#soft-target-semantics). |
| `modbus_active` | bool | `true` if a Modbus TCP server write has been applied within the watchdog window. Used by the dashboard's `MB` badge and by external clients that want to verify their command was received. |
| `modbus_op_mod` | uint16 | Last `OpMod` value received from the Modbus server (`2424` = Default, `2289` = Force charge, `2290` = Force discharge). |
| `modbus_force_w` | int32 | Battery power currently being commanded under force-mode (`0` when the P controller is running). Negative = charging, positive = discharging. |
| `last_error` | string | Last error message produced by a read or write. |

## Commands

| Command | Payload | Effect |
|---|---|---|
| `sbse_controller/config_update` | full `config` object | Validates, writes NVS, hot-reloads live-tunable subset. Equivalent to `PUT /sbse_controller/config`. |
| `sbse_controller/active_config_update` | full `active_config` object | Validates, updates runtime fields. No flash write. Equivalent to `PUT /sbse_controller/active_config`. |
| `sbse_controller/force_release` | empty | Writes one 0 W setpoint and pauses the loop for 30 s. `mode → paused`. Clears any Modbus-driven force-mode (operator takeover). |
| `sbse_controller/resume` | empty | Ends a `force_release` pause early. No-op if not paused. Also clears any Modbus force-mode. |

## Trace history endpoint (HTTP only)

`GET /sbse_controller/history` returns the last ~5 minutes of (grid, battery,
setpoint, target) samples that the controller has captured at 1 Hz. The
dashboard fetches this once on page load to seed the live chart so a reload
doesn't blank the trace. The buffer is RAM-only (~3.6 KB); it does not
survive a controller reboot.

The wire format is:

```json
{
  "samples": [
    [age_ms, grid_w, battery_w, setpoint_w, target_w],
    ...
  ]
}
```

Samples are ordered oldest → newest. `age_ms` is the row's age in
milliseconds *at the moment of the HTTP response* — i.e. compute the
absolute timestamp as `now − age_ms`. This means the consumer doesn't need
NTP sync between the device and the client. The other four fields are
int16 (saturating). The endpoint is HTTP-only; the buffer is not exposed
via MQTT.

## Modbus TCP server (external control)

When `config.modbus_server_enabled = true`, the controller listens on `config.modbus_server_port` (default `502`) and accepts the **same WriteMultipleRegisters traffic that the WARP charger's "SMA Hybrid Inverter" battery class sends to a real Sunny Boy Storage**. The controller is the server; an external WARP / energy manager / scripting client is the client.

### Accepted registers

Only `WriteMultipleRegisters` (function code `16`) at exactly these two addresses is accepted. Everything else returns `IllegalFunction` or `IllegalDataAddress`. The unit ID must match `modbus_server_unit_id` (or it is `0`, meaning "accept any").

| Address | Length | Type | Field | Notes |
|---|---|---|---|---|
| `40236` | 2 | U32BE | `CmpBMS.OpMod` | `2424` = Default, `2289` = Battery charging, `2290` = Battery discharging. Any other value is treated as `2424`. |
| `40793` | 10 | 5 × U32BE | `CmpBMS.{BatChaMinW, BatChaMaxW, BatDchgMinW, BatDchgMaxW, GridWSpt}` | All in W. Min values are accepted but only used to recognise force-mode; the actual force power comes from the matching Max. |

### Mode mapping

The latest `OpMod` value (sticky between writes) is interpreted on every `40793` write:

| OpMod | Effect on the SBSE controller |
|---|---|
| `2424` (Default/Normal/Block/Block-Charge/Block-Discharge) | P controller stays in charge. `BatChaMaxW` → `active_config.max_charge_w`, `BatDchgMaxW` → `active_config.max_discharge_w`. `GridWSpt` → `active_config.target_grid_w` **only if `modbus_server_use_grid_spt = true`** (off by default — see below). Force-mode is cleared. |
| `2289` (Battery charging) | **Force charge.** P controller bypassed. Battery commanded at `−BatChaMaxW` W (charging). Mode reports `force_charge`. EMA on the setpoint, deadband, SoC clamps and `max_*` still apply on the output. |
| `2290` (Battery discharging) | **Force discharge.** P controller bypassed. Battery commanded at `+BatDchgMaxW` W (discharging). Mode reports `force_discharge`. Same output filters as above. |

Each `40793` write is mirrored into `active_config` immediately (visible at `GET /sbse_controller/active_config` and on the dashboard's sliders).

### GridWSpt handling (`modbus_server_use_grid_spt`)

The WARP charger's "SMA Hybrid Inverter" battery class always writes `GridWSpt = 0` regardless of the chosen mode. Mirroring that verbatim into `active_config.target_grid_w` would zero out the operator's configured target on every mode change — almost certainly not what you want. The default `modbus_server_use_grid_spt = false` therefore ignores `GridWSpt` entirely; only `BatChaMaxW` / `BatDchgMaxW` and the `OpMod`-driven force-mode bits take effect. Switch the option on if you have a Modbus client that genuinely uses `GridWSpt` as a setpoint.

### Inverter feed-in limit caveat

The SBSE has a hard grid-feed-in limit configured directly on the inverter (e.g. 70 % of nameplate PV power, depending on grid-code rules). When PV instantaneously produces **more** than this limit, the inverter charges the battery from the excess **regardless of the 0 W setpoint we command**. This affects:

- "Block" mode (both caps zero) → battery still charges from PV that can't be exported.
- "Block Charge" mode → same.
- The `force_release` / "Pause 30 s" button → same.

In other words: while PV output is within the inverter's permitted feed-in range, a 0 W setpoint really idles the battery. Above that range, the battery absorbs the otherwise-curtailed PV no matter what the SBSE controller says. There is no fix at the controller level; raise the inverter's feed-in limit or accept the behaviour.

### Arbitration

"Last write wins" across **all** writers: Modbus, dashboard, HTTP `PUT /active_config`, and MQTT. An operator-driven `active_config` update, `force_release` or `resume` command also **drops force-mode and clears `modbus_active`** -- the operator is taking over. To re-engage, the Modbus client must send a fresh `40793` write.

### Watchdog

If the watchdog is enabled (`config.modbus_server_watchdog_s > 0`) and no Modbus write arrives within that many seconds, the live overrides (`target_grid_w`, `max_charge_w`, `max_discharge_w`) are reverted to the persistent `config` values and force-mode is cleared. SMA's own inverter watchdog runs at 5 minutes; the default of `60 s` here matches the resend interval that the WARP charger uses. Set to `0` to leave the most recent setpoint and force-mode in place indefinitely.

### Example: cURL force-charge at 1500 W

```bash
# 40236: OpMod = 2289 (force charge)  →  bytes: 00 00 08 F1
# 40793: BatChaMin = 1500, BatChaMax = 1500, rest zero  →  the relevant U32BE pair: 00 00 05 DC
# Easier: use modbus-cli or pymodbus rather than crafting raw frames by hand.
```

### Example: pymodbus force-charge at 1500 W

```python
from pymodbus.client import ModbusTcpClient
c = ModbusTcpClient("192.168.110.151", port=502)
c.connect()

# Step 1: OpMod = 2289 (force charge)
c.write_registers(40236, [0, 2289], unit=3)

# Step 2: BatChaMin = BatChaMax = 1500, others zero
c.write_registers(40793, [
    0, 1500,   # BatChaMinW
    0, 1500,   # BatChaMaxW
    0, 0,      # BatDchgMinW
    0, 0,      # BatDchgMaxW
    0, 0,      # GridWSpt
], unit=3)

c.close()
```

To release: write `OpMod = 2424` then `40793` with all-zeros (or non-zero `BatChaMaxW` / `BatDchgMaxW` for normal P-controller operation under those caps), or just send any `active_config` change from the dashboard.

## Soft target semantics

When `active_config.soft_target = true`, the P controller stops chasing
`target_grid_w` symmetrically. Instead it implements an **asymmetric
deadzone on the grid value**:

- If the grid would end up **more negative than `min(target, 0)`**
  (over-exporting beyond the configured target) → charge the battery to
  bring the grid up to `min(target, 0)`.
- If the grid would end up **more positive than `0`** (would import) →
  discharge the battery to bring the grid down to `0` (and no further --
  the controller does **not** discharge to push the grid all the way to
  a negative `target_grid_w`).
- If the grid is in the deadzone `[min(target, 0), 0]` → battery is
  commanded to `0 W`. The output EMA softens the transition; the inverter
  may still ramp gently from a previous non-zero setpoint.

User intent: "excess PV first goes to the grid up to my configured target,
the rest charges the battery; if the house out-draws PV, the battery
discharges only enough to avoid grid import."

### Examples (`target_grid_w = −200 W`)

| Grid without battery action | Hard mode (`soft_target = false`) | Soft mode (`soft_target = true`) |
|---|---|---|
| `−500 W` (PV excess of 500) | charge 300 W → grid `−200` | charge 300 W → grid `−200` *(identical)* |
| `−100 W` (PV excess of 100) | discharge 100 W → grid `−200` | **battery idle → grid `−100`** *(no over-export-hunt)* |
| `0 W` | discharge 200 W → grid `−200` | **battery idle → grid `0`** |
| `+500 W` (PV deficit of 500) | discharge 700 W → grid `−200` | **discharge 500 W → grid `0`** *(cover load, no further)* |

### Restrictions

- A **positive** `target_grid_w` is treated as `0` in soft mode (no
  autonomous grid charging). To intentionally import from the grid,
  switch back to hard mode.
- Modbus force-mode (`OpMod 2289` / `2290`) **bypasses** soft mode --
  external force commands are explicit overrides and take precedence.
- Other clamps (`max_charge_w`, `max_discharge_w`, SoC limits, output EMA,
  deadband, safety-zero) all apply unchanged on top of the soft logic.

## Controller loop in one diagram

```
each tick (default 300 ms, gated by `enabled` + connection + `paused`):

  measured_grid   = read GRID_POWER_ADDR (unit 2)
  battery_now     = read BATTERY_POWER_ADDR (unit 3)        ── if either fails, skip cycle
  soc             = read BATTERY_SOC_ADDR (unit 3, if due)     and increment read_fail_streak

  ema_grid       ← α_grid · measured_grid  + (1 − α_grid) · ema_grid
  d_ema_grid     = ema_grid − previous_ema_grid    (0 on the first cycle)

  if modbus_force_w != 0:                          ── SMA OpMod 2289 / 2290
    raw_setpoint = modbus_force_w                  ── (P+D bypassed)
  elif soft_target:                                ── asymmetric deadzone
    lo = min(target_grid_w, 0)
    if   ema_grid < lo:   delta = ema_grid − lo  ; raw_setpoint = battery_now + Kp·delta + Kd·d_ema_grid
    elif ema_grid > 0 :   delta = ema_grid       ; raw_setpoint = battery_now + Kp·delta + Kd·d_ema_grid
    else              :   raw_setpoint = 0       ── battery idle in deadzone
  else:                                            ── hard target
    delta        = ema_grid − target_grid_w
    raw_setpoint = battery_now + Kp · delta + Kd · d_ema_grid

  raw_setpoint   = clamp(raw_setpoint, by SoC: 100 % blocks charge, 0 % blocks discharge)
  raw_setpoint   = clamp(raw_setpoint, −max_charge_w, +max_discharge_w)
  ema_setpoint   ← α_set · raw_setpoint + (1 − α_set) · ema_setpoint
  target_w       = round(ema_setpoint)

  trace_history.add_sample(grid_ema, battery_now, last_written_w, target_grid_w)  ── 1 Hz internal throttle

  if |target_w − last_written_w| < deadband_w → skip write (inverter holds last setpoint)
  else                                          → write target_w to POWER_SETPOINT_ADDR (unit 3)

  if read_fail_streak == safety_zero_after_failures → write 0 W, mode = safety
  if modbus_active and last Modbus write > watchdog_s ago → revert overrides
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
| Charge from grid (low tariff) | `target_grid_w` | Positive value; only effective while `battery_soc < 100`. Requires `soft_target = false` (positive targets are treated as `0` in soft mode). |
| "PV-only" battery use (never discharge to chase a negative target) | `soft_target` | `true`. Battery still discharges to cover load, but stops at grid `= 0` instead of overshooting toward a negative target. |
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

# Force the loop to release control for 30 s, then resume early:
curl -X POST http://<sbse-controller-ip>/sbse_controller/force_release
curl -X POST http://<sbse-controller-ip>/sbse_controller/resume

# Fetch the last 5 minutes of (grid, battery, setpoint, target) at 1 Hz:
curl http://<sbse-controller-ip>/sbse_controller/history | jq '.samples | length'

# Watch the controller:
watch -n1 'curl -s http://<sbse-controller-ip>/sbse_controller/state | jq'
```

## Useful MQTT examples

Assuming the MQTT module is configured with topic prefix `sbse/` and a
broker is reachable from your machine:

```bash
# Watch live state:
mosquitto_sub -h <broker> -t 'sbse/sbse_controller/state'

# Watch active config (publishes on every change):
mosquitto_sub -h <broker> -t 'sbse/sbse_controller/active_config'

# Live-update one field (full object required by force_same_keys):
mosquitto_pub -h <broker> -t 'sbse/sbse_controller/active_config_update' \
              -m "$(mosquitto_sub -h <broker> -t 'sbse/sbse_controller/active_config' -C 1 \
                    | jq '.target_grid_w = 200')"

# Trigger force_release / resume:
mosquitto_pub -h <broker> -t 'sbse/sbse_controller/force_release' -m '{}'
mosquitto_pub -h <broker> -t 'sbse/sbse_controller/resume'        -m '{}'
```

The history endpoint is HTTP-only — large per-response payload (~15 kB)
that's wasteful to publish on every chart refresh.
