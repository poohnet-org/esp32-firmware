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
| `modbus_server_authority` | `1` (= caps) | uint8, 0…2 | How much of the operator's `active_config` an external Modbus client is allowed to overwrite on every WriteMultipleRegisters at register 40793. Live-tunable. See [Modbus authority](#modbus-authority-modbus_server_authority) below for the per-value behaviour table. |

## Live-tunable fields (in both `config` and `active_config`)

| Field | Default | Units / range | Purpose |
|---|---:|---|---|
| `grid_charge_target_w` | `0` | W, −750…+2500 | **Lower grid bound.** When the grid would go below this value (i.e. exporting more than the operator is comfortable with), the controller charges the battery to bring the grid back up to this value. |
| `grid_discharge_target_w` | `0` | W, −750…+2500, **must be ≥ `grid_charge_target_w`** | **Discharge rescue threshold.** When the grid drifts above this value, the controller starts discharging the battery to bring the grid back down to `hi`. Below `hi` the controller chases `grid_charge_target_w` instead, by charging only. See [Grid targets](#grid-targets) below. |
| `max_charge_w` | `5000` | W, 0…10 000 | **Battery saturation limit, charge direction.** Hard cap on charging power. The computed setpoint is clamped to `[−max_charge_w, +max_discharge_w]` before being written. `0` disables charging entirely. Orthogonal to the grid targets. |
| `max_discharge_w` | `5000` | W, 0…10 000 | **Battery saturation limit, discharge direction.** Hard cap on discharging power. `0` disables discharging entirely. Orthogonal to the grid targets. |
| `kp_milli` | `1000` (= Kp 1.0) | Kp × 1000, 100…2000 | **Proportional controller gain.** Per tick (outside the deadzone): `new_setpoint = battery_now + Kp · (ema_grid − target) + Kd · Δema_grid`, where `target` is whichever bound the controller is chasing. `Kp = 1.0` ≈ "one-shot correction." Lower (~0.5) = slower, more damped. Higher (~1.5) = snappier but can ring against the EMA filter. |
| `kd_milli` | `0` (= Kd 0.0, disabled) | Kd × 1000, 0…3000 | **Derivative gain on the smoothed grid measurement** (`Δema_grid = ema_grid − previous_ema_grid` per tick). Derivative-on-measurement avoids a kick when a grid target changes. Anticipates fast load steps that the EMA would otherwise see only after one or two cycles of lag. Set to `0` for the classic P + implicit-I controller. Raise toward `1.0`–`2.0` if induction plates / kettles cause overshoot. Too high → ringing. |
| `alpha_grid_milli` | `300` (= α 0.30) | α × 1000, 10…1000 | **EMA on the grid-power input.** `ema = α · measured + (1−α) · ema`. `α = 0.30` averages roughly the last 3–5 samples. `α → 1.0` disables smoothing; `α → 0` is heavy damping with lag. |
| `alpha_setpoint_milli` | `700` (= α 0.70) | α × 1000, 10…1000 | **EMA on the commanded battery setpoint** (post-controller, pre-write). Smooths the output to the inverter so noisy grid readings don't produce flickery commands. |
| `deadband_w` | `50` | W, 0…1000 | **Write-suppression threshold.** If the new computed setpoint is within ±`deadband_w` of the last commanded one, the Modbus write is skipped (inverter holds the last value). Cuts cell churn and bus traffic at idle. SBSE has no internal-control fallback, so generous deadbands are safe. |
| `safety_zero_after_failures` | `5` | count, 0…100 | **Safety net.** After this many consecutive failed read cycles, the controller commands a one-shot 0 W setpoint and enters `mode: safety` until reads recover. At default 300 ms tick × 5 → ~1.5 s trip latency. `0` disables the safety net (last setpoint held forever during outages). |

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
| `modbus_active` | bool | `true` if a Modbus TCP server write has been applied within the watchdog window. Used by the dashboard's `MB` badge and by external clients that want to verify their command was received. |
| `modbus_op_mod` | uint16 | Last `OpMod` value received from the Modbus server (`2424` = Default, `2289` = Force charge, `2290` = Force discharge). |
| `modbus_force_w` | int32 | Battery power currently being commanded under force-mode (`0` when the P controller is running). Negative = charging, positive = discharging. |
| `last_error` | string | Last error message produced by a read or write. |

## Commands

| Command | Payload | Effect |
|---|---|---|
| `sbse_controller/config_update` | full `config` object | Validates, writes NVS, hot-reloads live-tunable subset. Equivalent to `PUT /sbse_controller/config`. |
| `sbse_controller/active_config_update` | full `active_config` object | Validates, updates runtime fields. No flash write. Equivalent to `PUT /sbse_controller/active_config`. |
| `sbse_controller/pause` | empty | Writes one 0 W setpoint and pauses the loop for 30 s. `mode → paused`. Clears any Modbus-driven force-mode (operator takeover). |
| `sbse_controller/resume` | empty | Ends a `pause` early. No-op if not paused. Also clears any Modbus force-mode. |

## Trace history endpoint (HTTP only)

`GET /sbse_controller/history` returns the last ~5 minutes of (grid, battery,
setpoint, charge target, discharge target) samples that the controller has
captured at 1 Hz. The dashboard fetches this once on page load to seed the
live chart so a reload doesn't blank the trace. The buffer is RAM-only
(~4.2 KB); it does not survive a controller reboot.

The wire format is:

```json
{
  "samples": [
    [age_ms, grid_w, battery_w, setpoint_w, target_lo_w, target_hi_w],
    ...
  ]
}
```

Samples are ordered oldest → newest. `age_ms` is the row's age in
milliseconds *at the moment of the HTTP response* — i.e. compute the
absolute timestamp as `now − age_ms`. This means the consumer doesn't need
NTP sync between the device and the client. `target_lo_w` /
`target_hi_w` are the grid deadzone bounds at capture time
(`grid_charge_target_w` and `grid_discharge_target_w`); they coincide in
hard mode and differ in soft mode. The other four numeric fields are
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
| `2424` (Default/Normal/Block/Block-Charge/Block-Discharge) | P controller stays in charge. Which of `BatChaMaxW` / `BatDchgMaxW` / `GridWSpt` actually land in `active_config` depends on `modbus_server_authority` -- see [Modbus authority](#modbus-authority-modbus_server_authority) below. Force-mode is cleared. |
| `2289` (Battery charging) | **Force charge.** P controller bypassed. Battery commanded at `−BatChaMaxW` W (charging). Mode reports `force_charge`. EMA on the setpoint, deadband, SoC clamps and `max_*` still apply on the output. |
| `2290` (Battery discharging) | **Force discharge.** P controller bypassed. Battery commanded at `+BatDchgMaxW` W (discharging). Mode reports `force_discharge`. Same output filters as above. |

Each `40793` write is mirrored into `active_config` immediately (visible at `GET /sbse_controller/active_config` and on the dashboard's sliders).

### Modbus authority (`modbus_server_authority`)

How much of the operator's `active_config` an external Modbus client is allowed to overwrite on each `40793` write. The SMA `OpMod` force-mode commands (`2289` / `2290`) are **always** honoured -- this setting only governs the persistent caps and grid targets.

| Value | What each Modbus write applies to `active_config` | Operator-preserved | Typical use |
|---:|---|---|---|
| `0` `force_only` | nothing | `max_charge_w`, `max_discharge_w`, `grid_charge_target_w`, `grid_discharge_target_w` | I just want external force commands (peak shaving, grid services); the rest of my configuration is mine. |
| `1` `caps` (default) | `BatChaMaxW` → `max_charge_w`, `BatDchgMaxW` → `max_discharge_w`. `GridWSpt` ignored. | `grid_charge_target_w`, `grid_discharge_target_w` | A WARP charger / external EM does battery management for me, but I set the grid target myself. |
| `2` `full` | All three: caps as above, plus `GridWSpt` mirrored into **both** grid targets (hard-mode chase). | nothing | An upstream controller is fully in charge of my battery and grid setpoint. |

A few semantic notes:

- **Force-mode is independent of authority.** When `OpMod = 2289` (force charge), the controller commands the battery at `−BatChaMaxW` regardless of authority; when `OpMod = 2290`, it commands `+BatDchgMaxW`. The operator's `max_charge_w` / `max_discharge_w` always clamp the actual write (output saturation), so even in `force_only` mode the operator's caps win over the requested force power.
- **Caps and grid target are *not* unified.** `caps` applies caps but never overwrites the grid targets, even if WARP sends them. `full` applies both. There is no "grid target only" mode (no realistic use case asked for it).
- **Watchdog revert** (`modbus_server_watchdog_s`) restores both caps and grid targets from the persistent `config` regardless of authority -- it's about recovering when the Modbus client disappears, not about authority delegation.

### Inverter feed-in limit caveat

The SBSE has a hard grid-feed-in limit configured directly on the inverter (e.g. 70 % of nameplate PV power, depending on grid-code rules). When PV instantaneously produces **more** than this limit, the inverter charges the battery from the excess **regardless of the 0 W setpoint we command**. This affects:

- "Block" mode (both caps zero) → battery still charges from PV that can't be exported.
- "Block Charge" mode → same.
- The `pause` command / "Pause 30 s" button → same.

In other words: while PV output is within the inverter's permitted feed-in range, a 0 W setpoint really idles the battery. Above that range, the battery absorbs the otherwise-curtailed PV no matter what the SBSE controller says. There is no fix at the controller level; raise the inverter's feed-in limit or accept the behaviour.

### Arbitration

"Last write wins" across **all** writers: Modbus, dashboard, HTTP `PUT /active_config`, and MQTT. An operator-driven `active_config` update, `pause` or `resume` command also **drops force-mode and clears `modbus_active`** -- the operator is taking over. To re-engage, the Modbus client must send a fresh `40793` write.

### Watchdog

If the watchdog is enabled (`config.modbus_server_watchdog_s > 0`) and no Modbus write arrives within that many seconds, the live overrides (`grid_charge_target_w`, `grid_discharge_target_w`, `max_charge_w`, `max_discharge_w`) are reverted to the persistent `config` values and force-mode is cleared. SMA's own inverter watchdog runs at 5 minutes; the default of `60 s` here matches the resend interval that the WARP charger uses. Set to `0` to leave the most recent setpoint and force-mode in place indefinitely.

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

## Grid targets

The controller has **two** grid-side targets that play different roles:

- `lo = grid_charge_target_w` — the **primary target**. The controller
  always tries to reach this value by *charging* the battery.
- `hi = grid_discharge_target_w` — the **rescue threshold**. The
  controller only acts on `hi` when the grid drifts above it; it then
  starts *discharging* the battery to bring the grid back down to `hi`.

The validator enforces `hi ≥ lo`. The dashboard renders a `HARD` badge
when `lo == hi` and a `SOFT` badge when `lo < hi`; that's a derived
signal, not a separate configuration.

### Hard mode (`lo == hi`)

Symmetric P + implicit-I + D chase of a single value, in both
directions. Identical to the legacy `target_grid_w` semantics. The
controller will charge *or* discharge as needed to hit the target.

### Soft mode (`lo < hi`)  -- asymmetric

| Where is `ema_grid`? | What the controller does |
|---|---|
| `ema_grid > hi` | **Chase `hi` by discharging.** Standard P + I + D law with `hi` as the target. Discharge is allowed up to `max_discharge_w`. |
| `ema_grid ≤ hi` | **Chase `lo` by charging only.** Same P + I + D law with `lo` as the target, but the output is clamped to `≤ 0` -- the controller is forbidden from discharging just to pursue `lo`. If charging less than 0 won't reach `lo` (i.e. PV is too weak), the battery simply stays at 0 and the grid drifts up rather than burning battery to pad the export. |

This implements your stated intent:

> "Excess PV power first goes to the grid until reaching the configured
> target value and the remaining PV power goes to the battery. If the
> house draws more power than PV can deliver, then the battery should be
> discharged before drawing power from the grid."

Mapping:
- "excess PV first goes to the grid up to the target": that's `lo` and
  is chased actively by charging.
- "remaining PV goes to the battery": natural consequence of chasing
  `lo` (when PV surplus is large, charging is the only way to keep grid
  at `lo`).
- "battery discharged before drawing power from the grid": that's the
  `hi` rescue. Typically `hi = 0` ("don't import"); a non-zero `hi`
  means "I'm willing to lose up to `−hi W` of net export before
  the battery starts discharging to keep at least that much export
  going" (or, for positive `hi`, "I tolerate up to `hi W` of import
  before the battery kicks in").

The boundary at `hi` is smooth in the typical steady-state transition
(battery is near 0 by the time grid reaches `hi` from below, so both
branches agree). Fast load steps that jump across `hi` while the battery
is still actively charging see a small step in the commanded setpoint;
the output EMA softens it.

### Examples

#### `lo = hi = 0`  (default: pure self-consumption)

The controller chases grid = 0 in both directions. Any PV surplus →
battery charges. Any house deficit → battery discharges. Grid stays
near 0.

#### `lo = -200, hi = 0`  (export-preferred self-consumption)

| Grid without battery action | Result |
|---|---|
| `-500 W` (PV surplus of 500) | charge 300 W → grid `-200` *(chasing `lo`)* |
| `-100 W` (small PV surplus) | chase `lo` wants discharge → **clamped, battery stays at 0** → grid stays `-100` |
| `0 W` (balanced) | chase `lo` wants discharge → **clamped, battery stays at 0** → grid stays `0` |
| `+500 W` (PV deficit of 500) | discharge 500 W → grid `0` *(chasing `hi`, **no further into export**)* |

#### `lo = -720, hi = -500`  (maintain export, with a 500 W discharge rescue)

| Grid without battery action | Result |
|---|---|
| `-900 W` (large PV surplus) | charge 180 W → grid `-720` *(chasing `lo`)* |
| `-700 W` | battery active and charging → ramps to charge a touch more → grid drops to `-720` |
| `-600 W` (PV starting to drop) | chase `lo` wants less charge; battery ramps toward 0; grid drifts up |
| `-500 W` (PV further down) | battery has reached 0; chase `lo` clamped → grid drifts above `hi` |
| `-400 W` (PV well below the export target) | chase `hi` engages → discharge 100 W → grid back to `-500` |

This is your live setup. The `lo` target is "ideal export"; `hi` is
"minimum acceptable export before the battery starts paying for it".

#### `lo = hi = -200`  (hard: maintain 200 W export at all costs)

| Grid without battery action | Result |
|---|---|
| `-500 W` | charge 300 W → grid `-200` |
| `-100 W` | discharge 100 W → grid `-200` *(symmetric chase, hard mode allows it)* |
| `+500 W` | discharge 700 W → grid `-200` |

#### `lo = 0, hi = +500`  (allow up to 500 W import before discharging)

| Grid without battery action | Result |
|---|---|
| `-100 W` (small PV surplus) | charge 100 W → grid `0` *(absorb surplus, don't export)* |
| `+200 W` (importing 200) | chase `hi` wants no action (already below) → **battery stays at 0** → grid stays `+200` |
| `+800 W` (importing 800) | discharge 300 W → grid `+500` *(chasing `hi`)* |

### Other interactions

- **Force-mode** (`OpMod 2289` / `2290` from the Modbus server) bypasses
  the whole target system -- the requested charge / discharge power is
  written directly. SoC clamps, output EMA and the saturation limits
  (`max_charge_w` / `max_discharge_w`) still apply on top.
- The saturation limits (`max_charge_w` / `max_discharge_w`) are
  orthogonal to the grid targets: an "unreachable" grid target (e.g.
  `lo = -1000` with `max_charge_w = 100`) is not an error; the
  controller saturates at the limit and the grid balances wherever it
  ends up.
- Modbus writes to register `40793` only update the grid targets in the
  `full` authority mode; in the default `caps` mode the operator's
  targets are preserved. See [Modbus authority](#modbus-authority-modbus_server_authority).

## Controller loop in one diagram

```
each tick (default 300 ms, gated by `enabled` + connection + `paused`):

  measured_grid   = read GRID_POWER_ADDR (unit 2)
  battery_now     = read BATTERY_POWER_ADDR (unit 3)        ── if either fails, skip cycle
  soc             = read BATTERY_SOC_ADDR (unit 3, if due)     and increment read_fail_streak

  ema_grid       ← α_grid · measured_grid  + (1 − α_grid) · ema_grid
  d_ema_grid     = ema_grid − previous_ema_grid    (0 on the first cycle)

  lo = grid_charge_target_w
  hi = grid_discharge_target_w           ── validator: hi >= lo

  if modbus_force_w != 0:                          ── SMA OpMod 2289 / 2290
    raw_setpoint = modbus_force_w                  ── (P+D bypassed)
  elif lo == hi:                                   ── hard mode: symmetric chase
    delta        = ema_grid − lo
    raw_setpoint = battery_now + Kp · delta + Kd · d_ema_grid
  elif ema_grid > hi:                              ── soft, rescue: chase hi via discharge
    delta        = ema_grid − hi
    raw_setpoint = battery_now + Kp · delta + Kd · d_ema_grid
  else:                                            ── soft, primary: chase lo via charge
    delta        = ema_grid − lo
    raw_setpoint = battery_now + Kp · delta + Kd · d_ema_grid
    if raw_setpoint > 0: raw_setpoint = 0          ── never discharge to chase lo

  raw_setpoint   = clamp(raw_setpoint, by SoC: 100 % blocks charge, 0 % blocks discharge)
  raw_setpoint   = clamp(raw_setpoint, −max_charge_w, +max_discharge_w)
  ema_setpoint   ← α_set · raw_setpoint + (1 − α_set) · ema_setpoint
  target_w       = round(ema_setpoint)

  trace_history.add_sample(grid_ema, battery_now, last_written_w, lo, hi)  ── 1 Hz internal throttle

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
| Pure self-consumption | `grid_charge_target_w` = `grid_discharge_target_w` = `0` | Default. Battery balances grid to 0 in both directions. |
| Dump excess to grid (hard chase) | both targets = `-200` | Maintain 200 W export at all costs, including by discharging battery. |
| Allow export, never discharge to chase it | `grid_charge_target_w = -200`, `grid_discharge_target_w = 0` | Export up to 200 W when PV allows; if house out-draws PV, discharge only to grid = 0 (no further). |
| Charge from grid at a fixed rate (low tariff) | both targets = `+200` | Maintain +200 W import (battery absorbs the imported power). Only effective while `battery_soc < 100`. |
| Allow some import before discharging kicks in | `grid_charge_target_w = 0`, `grid_discharge_target_w = +500` | Battery doesn't fight small grid imports up to 500 W; above 500 W it discharges to cap. |
| Battery keeps its last setpoint during a Modbus outage | `safety_zero_after_failures` | Set to `0` (disables the safety-zero trip). |

## Useful curl examples

```bash
# Read current effective values:
curl http://<sbse-controller-ip>/sbse_controller/active_config

# Live update one field (GET → modify → PUT). Note: this sets the upper
# bound to 200 (allow up to 200 W of grid import before discharging) while
# leaving the lower bound alone.
curl http://<sbse-controller-ip>/sbse_controller/active_config \
  | jq '.grid_discharge_target_w = 200' \
  | curl -X PUT http://<sbse-controller-ip>/sbse_controller/active_config \
         -H 'Content-Type: application/json' -d @-

# Change the persistent boot default (writes flash, also hot-reloads):
curl http://<sbse-controller-ip>/sbse_controller/config \
  | jq '.deadband_w = 100' \
  | curl -X PUT http://<sbse-controller-ip>/sbse_controller/config \
         -H 'Content-Type: application/json' -d @-

# Pause the loop for 30 s, then resume early:
curl -X POST http://<sbse-controller-ip>/sbse_controller/pause
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
                    | jq '.grid_discharge_target_w = 200')"

# Trigger pause / resume:
mosquitto_pub -h <broker> -t 'sbse/sbse_controller/pause'  -m '{}'
mosquitto_pub -h <broker> -t 'sbse/sbse_controller/resume' -m '{}'
```

The history endpoint is HTTP-only — large per-response payload (~15 kB)
that's wasteful to publish on every chart refresh.
