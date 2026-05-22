# `sbse_controller` architecture

Developer-oriented map of the module. For user-facing config / state /
command reference see [`CONFIG.md`](CONFIG.md).

## What this module is

A self-contained ESP32 firmware module that:

1. Talks Modbus TCP to an **SMA Sunny Boy Smart Energy (SBSE) hybrid
   inverter** as a *client*, reading grid power, battery power and SoC at
   ~300 ms and writing back an active-power setpoint for the battery
   inverter.
2. Computes that setpoint with a **P + implicit-I + D-on-measurement**
   control law so the grid stays inside a user-chosen `[lo, hi]` deadzone
   (with `lo == hi` collapsing to single-target chase).
3. Exposes its full surface (state, config, commands) over **HTTP, MQTT
   and the built-in dashboard** under identical semantics.
4. Optionally listens for **SMA-compatible Modbus TCP writes** so an
   external controller (e.g. a WARP charger configured for "SMA Hybrid
   Inverter") can steer it.

SBSE-only by design: the register map (addresses, byte order, the
`-15000` companion value, etc.) is hard-coded. Other inverters would
need a different module.

## File layout

```
src/modules/sbse_controller/
  ARCHITECTURE.md           this file
  CONFIG.md                 user-facing reference

  module.ini                Requires/After deps (build-system input)

  sbse_controller.h         class declaration + helper-class members
  sbse_controller.cpp       module skeleton + SMA OpMod semantics
  sbse_control_loop.cpp     tick pipeline + register map
  sbse_modbus_server.{h,cpp}  SMA-compatible Modbus server (protocol)
  sbse_modbus_proxy.{h,cpp}   SMA-hybrid read-side register cache
  sbse_trace_history.{h,cpp}  5-min 1 Hz ring buffer (HTTP endpoint)

web/src/modules/sbse_controller/
  api.ts                    TypeScript interfaces for config/state
  main.tsx                  Preact components (status card, sub-page, chart)
  post.scss                 dashboard styles + dark-mode overrides
  translation_en.tsx        EN labels and help text (JSX strings)
  translation_de.tsx        DE labels and help text
```

The split below the C++ level is by **responsibility**, not size.

## "Where does X live?" lookup

| Concern | File |
|---|---|
| Module lifecycle (pre_setup / setup / register_urls / register_events / pre_reboot) | `sbse_controller.cpp` |
| `config` + `active_config` schema, validators, hot-reload plumbing | `sbse_controller.cpp` |
| `pause` / `resume` command handlers | `sbse_controller.cpp` |
| SMA `OpMod` constants and sticky state, force-mode interpretation, `apply_modbus_setpoint_block`, watchdog | `sbse_controller.cpp` |
| Per-tick read → compute → write pipeline (`tick`, `read_*`, `compute_and_write`, `send_*`) | `sbse_control_loop.cpp` |
| SBSE register-map constants (`GRID_POWER_ADDR`, `BATTERY_POWER_ADDR`, `SBSE_COMPANION_VALUE`, …) | `sbse_control_loop.cpp` |
| `mode_name(Mode)` translation table | `sbse_control_loop.cpp` |
| `current_running_mode()` (chooses `running` / `force_*` / `block_*` / `blocked`) | `sbse_control_loop.cpp` |
| `connect_callback` / `disconnect_callback` (Modbus *client* connection events) | `sbse_control_loop.cpp` |
| Listener socket, dispatch, unit-id filter for the *server* | `sbse_modbus_server.{h,cpp}` |
| SMA-hybrid read-side register cache + round-robin upstream poll | `sbse_modbus_proxy.{h,cpp}` |
| Trace ring buffer, 1 Hz throttle, `GET /sbse_controller/history` | `sbse_trace_history.{h,cpp}` |

## Class layout

```
                   ┌───────────────────────────┐
                   │      SbseController       │
                   │  (extends IModule,        │
                   │   GenericTCPClientPool…)  │
                   └─────────────┬─────────────┘
                                 │ owns by value
       ┌──────────────┬──────────┴──────────┬──────────────────────────┐
       │              │                     │                          │
┌──────▼───────────┐ ┌▼──────────────────┐ ┌▼─────────────────┐ ┌──────▼─────────────────┐
│ SbseModbusServer │ │ SbseModbusProxy   │ │ SbseTraceHistory │ │ TFModbusTCPSharedClient│
│ (network adapter)│ │ (read-side cache  │ │ (5-min ring +    │ │  (Modbus client to     │
│                  │ │  + poll groups)   │ │  /history GET)   │ │  SBSE inverter, owned  │
└──────────────────┘ └───────────────────┘ └──────────────────┘ │  by the shared pool)   │
                                                                └────────────────────────┘
```

`SbseController` is the only class with knowledge of the *semantics*
(active_config, force-mode, OpMod, watchdog timing, P controller). The
two helper classes know only their narrow concerns.

## Helper-class contracts

### `SbseTraceHistory`

```cpp
void add_sample(int32_t grid_w, int32_t battery_w,
                int32_t setpoint_w, int32_t target_w);
void register_url(const char *path);
```

- Self-contained. No callbacks back into the controller.
- Internal 1 Hz throttle: call as often as you like (e.g. every
  300 ms tick), it drops the in-between calls.
- Each sample is `(micros_t captured_us, int16_t × 4 channels)` = 12 B.
- `CAPACITY = 300` → 3.6 KB RAM.

### `SbseModbusServer`

```cpp
using OpModHandler    = std::function<TFModbusTCPExceptionCode(uint32_t op_mod)>;
using SetpointHandler = std::function<TFModbusTCPExceptionCode(uint16_t start_address,
                                                               uint16_t reg_count,
                                                               const uint16_t *regs)>;
using ReadHandler     = std::function<TFModbusTCPExceptionCode(TFModbusTCPFunctionCode fc,
                                                               uint16_t start_address,
                                                               uint16_t reg_count,
                                                               uint16_t *out_regs)>;

void set_handlers(OpModHandler, SetpointHandler, ReadHandler);
void configure(bool enabled, uint16_t port, uint8_t unit_id);
bool needs_restart_for(bool new_enabled, uint16_t new_port) const;
void start();   void stop();   void restart();
```

- Pure protocol adapter. Accepts WriteMultipleRegisters at `40236`
  (length 2) and any even-aligned 2/4/6/8/10-register sub-block within
  `40793..40802`; plus ReadHoldingRegisters / ReadInputRegisters at any
  address (delegated wholesale to `on_read`). Anything else →
  `IllegalFunction` / `IllegalDataAddress`.
- Knows nothing about `active_config`, force-mode, the watchdog, or
  the SMA OpMod *values* (it just hands the raw uint32 to the handler).
- Owns the underlying `TFModbusTCPServer`, its 20 ms tick task, and
  the most-recent (enabled, port) pair for `needs_restart_for()`.

The controller wires the handlers in `register_events()`:

```cpp
modbus_server.set_handlers(
    [this](uint32_t op_mod) { return this->on_modbus_op_mod_write(op_mod); },
    [this](uint16_t addr, uint16_t count, const uint16_t *regs) {
        return this->on_modbus_setpoint_write(addr, count, regs);
    },
    [this](TFModbusTCPFunctionCode fc, uint16_t addr, uint16_t count, uint16_t *out) {
        return this->on_modbus_read(fc, addr, count, out);
    });
```

### `SbseModbusProxy`

```cpp
struct SynthesisInputs {
    int32_t grid_w_raw;       // matches SMA GridMs.TotW sign (positive = import)
    int32_t battery_w_raw;    // positive = discharging, negative = charging
    uint8_t soc_pct;          // 255 = unknown -> NaN
};

void invalidate_all();
bool next_poll(size_t *idx, uint8_t *unit, uint16_t *addr,
               uint16_t *count, uint16_t **cache_dst);
void mark_group_done(size_t idx, bool success);

TFModbusTCPExceptionCode pack_response(TFModbusTCPFunctionCode fc,
                                       uint16_t start_addr,
                                       uint16_t count,
                                       uint16_t *response_buf,
                                       const SynthesisInputs &inputs) const;
```

- Holds the read-side register set evcc's `sma-hybrid` template
  queries: cumulative energy totals, per-phase grid W/A, PV DC. Some
  registers are synthesized from controller state at read time
  (`GridMs.TotW`, `Bat.ChaStt`, `BatChrg.CurBatCha`, `BatDsch.CurBatDsch`),
  the rest are populated by a round-robin polling cycle the controller
  drives every 500 ms via the existing modbus client connection.
- `pack_response()` is the server's `on_read` handler; uncovered
  addresses get the per-type SMA NaN sentinel (`0xFFFFFFFF` for
  uint32/uint64, `0x80000000` for int32) so the client decodes them
  as "no data" rather than a misleading 0.
- See the group table at the top of `sbse_modbus_proxy.cpp` for the
  full address → cache layout / upstream-unit mapping.

## Lifecycle (Tinkerforge `IModule`)

```
pre_setup()          schema-define config / active_config / state
setup()              restorePersistentConfig, seed caches, wire UI
register_urls()      api.addState/addCommand bindings, MQTT mirroring,
                     custom GET /sbse_controller/history endpoint
register_events()    network-connect listener, schedule tick(),
                     set_handlers() + start() on the Modbus server
pre_reboot()         cancel tick, stop modbus_server, send_zero_w()
                     (best-effort 0 W to inverter), stop_connection()
```

Network-connect handling: the controller waits for the network module
to publish `network_connected = true` before kicking off the Modbus
client connection. The Modbus *server* binds `0.0.0.0:port` and doesn't
need to wait.

## One tick — data flow

(Default `tick_ms = 300`; one cycle per tick. Modbus-client transactions
chain via callbacks; the pool serialises them on this connection.)

```
tick()
 │
 ├── watchdog_tick()  ── reverts overrides if no Modbus traffic in watchdog_s
 │
 ├── begin_cycle()    ── gates on enabled / paused / connected / in_flight
 │
 ▼
read_grid_power()  ── unit 2, addr 31249 (int32be, negated → import positive)
 │ ok → grid_w_raw
 ▼
read_battery_power()  ── unit 3, addr 31585 (8 reg, 2 × U32 charge & discharge)
 │ ok → battery_w_raw = discharge − charge   (positive = discharging)
 ▼
read_soc()  (only when soc_interval_ms has elapsed)
 │
 ▼
compute_and_write()
 │
 │  ema_grid       ← α_grid · grid_w_raw + (1 − α_grid) · ema_grid
 │  d_ema_grid     = ema_grid − previous_ema_grid
 │
 │  lo = grid_charge_target_w   # primary target
 │  hi = grid_discharge_target_w  # discharge rescue threshold
 │
 │  if modbus_force_w != 0:                  # OpMod 2289 / 2290
 │     raw_setpoint = modbus_force_w         # (P + D bypassed)
 │  else:                                    # P + implicit-I + D, regime-aware
 │     natural_grid = ema_grid + battery_w_raw   # grid if battery=0 (= load − PV)
 │     target       = clamp(natural_grid, lo, hi)
 │     raw_setpoint = battery_w_raw + Kp·(ema_grid − target) + Kd·d_ema_grid
 │     # direction lock: each active regime only acts in its natural direction
 │     if natural_grid > hi and raw_setpoint < 0: raw_setpoint = 0   # don't charge to chase hi
 │     if natural_grid < lo and raw_setpoint > 0: raw_setpoint = 0   # don't discharge to chase lo
 │
 │  clamp by SoC (100 % blocks charge, 0 % blocks discharge)
 │  clamp by [-max_charge_w, +max_discharge_w]
 │  ema_setpoint   ← α_set · raw_setpoint + (1 − α_set) · ema_setpoint
 │
 │  trace_history.add_sample(...)      (1 Hz throttle inside)
 │
 │  # Keep-alive, two paths sharing keepalive_interval_s:
 │  #   idle pulse  -- if target_w would be 0 and battery has been idle
 │  #                  for the interval, override with ±keepalive_pulse_w.
 │  #   refresh     -- if last_write_ok is older than the interval, bypass
 │  #                  the deadband and re-assert target_w as-is.
 │  # (Both gated behind force-mode / pause / safety branches above.)
 │
 │  if !keepalive_pulse and !keepalive_refresh
 │     and |target − last_written_w| < deadband_w               → skip write
 │  else  → send_setpoint(target)              → write 41467 (4 reg)
 ▼
finish_cycle(mode)   ── publishes the mode pill on the dashboard
```

Read failures increment `read_fail_streak`. When the streak crosses
`safety_zero_after_failures`, `cycle_failed` arms a one-shot 0 W
"safety" write and the mode flips to `safety` until reads recover.

## Mode enum and how it surfaces

```cpp
enum class Mode : uint8_t {
    Disabled, NotConnected, Stale, Running, Faulted, Paused, Safety,
    ForceCharge, ForceDischarge,
    Blocked, BlockCharge, BlockDischarge,
};
```

- `Disabled` / `NotConnected` / `Stale` / `Faulted` / `Paused` / `Safety`
  reflect lifecycle / fault state.
- `Running` is the "controller is doing its job" case.
- `ForceCharge` / `ForceDischarge`: a Modbus client wrote OpMod
  2289 / 2290; the controller bypasses the P loop and commands
  `±BatChaMaxW` / `±BatDchgMaxW` directly.
- `Blocked` / `BlockCharge` / `BlockDischarge`: derived purely from
  `max_charge_w` / `max_discharge_w` being `0`. Source-independent — the
  dashboard slider going to 0 produces the same mode as a Modbus
  "Block Charge" write.

`current_running_mode()` (in `sbse_control_loop.cpp`) picks the right
mode every tick. It's the only place this logic lives.

**Hard vs. soft is derived, not stored.** The dashboard reads
`grid_charge_target_w` and `grid_discharge_target_w` from `active_config`
and shows a `HARD` badge if they're equal or a `SOFT` badge if they
differ. The mode pill (`running` / `block_*` / `force_*` / etc.) is
orthogonal to that derived signal.

## State ownership

| State | Owner | Mutation source |
|---|---|---|
| `config` (NVS-backed) | `SbseController::config` | HTTP/MQTT `config_update` only |
| `active_config` | `SbseController::active_config` | HTTP/MQTT/dashboard `active_config_update`, **plus** internal mutations from `apply_modbus_setpoint_block` (the Modbus-driven path bypasses the command handler to avoid clearing its own force-mode state) |
| Cached fast-path mirrors (`kp`, `kd`, `max_*`, `grid_charge_target_w`, `grid_discharge_target_w`, `alpha_*`, `deadband_w`) | `SbseController` members | `apply_runtime_from_active()` after any `active_config` change |
| `state` (read-only) | `SbseController::state` | One updater per field, scattered across the cycle. Auto-publishes via the API event bus. |
| `modbus_op_mod`, `modbus_force_w`, `modbus_active`, `last_modbus_write_us` | `SbseController` members | Modbus dispatch handlers + operator-takeover paths |
| Modbus server (listener, dispatch state) | `SbseModbusServer` | `configure()` / `start()` / `stop()` from controller |
| Trace ring buffer | `SbseTraceHistory` | `add_sample()` from control loop |

The "**source of truth**" pattern is: persistent config in `config`,
runtime values in `active_config`, hot-path values cached in plain
members, status in `state`. Each layer is updated only by a small,
well-defined set of callers.

## Arbitration: who wins between Modbus / HTTP / MQTT / dashboard

"**Last write wins**" across all four. The active_config command handler
is the operator-takeover path: any write through it (HTTP, MQTT, the
dashboard's Apply buttons) clears `modbus_force_w` / `modbus_active`
explicitly. Modbus dispatch mutates `active_config` *directly* (via
`Config::updateXxx`, which auto-publishes the event but skips the
command handler) so that its own state isn't immediately stomped.

`pause` and `resume` also clear Modbus state — they're explicit operator
takeover.

## Transport equivalence

Every endpoint is reachable identically through:

- HTTP (`/sbse_controller/...`) via the `Web Server` module
- MQTT (`<prefix>/sbse_controller/...`) via the `Mqtt` module
- The built-in dashboard (`main.tsx`)

The MQTT module auto-mirrors every `api.addState` and `api.addCommand`
registration, so adding a new field in `pre_setup()` makes it available
on all three surfaces with no extra wiring.

Exception: `GET /sbse_controller/history` is a custom `server.on()`
handler (large payload, not state-shaped) — HTTP only.

## Frontend

Preact + react-bootstrap. The status page (top-level "Status" card) is
the live trace + active_config sliders + commands; the sub-page is the
persistent config form.

| Element | Source |
|---|---|
| Module navbar entry + sub-page | `SbseControllerNavbar`, `SbseController` |
| Dashboard status card | `SbseControllerStatus` |
| Live chart (uPlot) | `SbseControllerChart` |
| Top-right header status badge | `register_status_provider("sbse_controller", ...)` in `init()` |
| Tile colours (charge/discharge, import/export) | `.sbse-tile-*` classes in `post.scss`, picked via the `accent` prop |
| Dark-mode colours | `[data-bs-theme="dark"]` overrides in `post.scss` |

`SbseControllerStatus.componentDidMount` fetches
`/sbse_controller/history` once and prepends the seeded samples to
whatever live samples already arrived through the
`sbse_controller/state` event listener (set up in the constructor). The
existing 5-minute window cutoff trims old samples as the chart slides.

## Build dependencies

`module.ini` declares (`Requires`):

- `Task Scheduler` — for the per-tick scheduling and Modbus server tick
- `Event Log` — for `logger.printfln`
- `API` — for `api.addState` / `api.addCommand`
- `Event` — for `network.on_network_connected`
- `Network` — networking core
- `Network Lib` — `GenericTCPClientPoolConnector`
- `Modbus TCP Client` — the shared client pool we use as a *client* of the inverter
- `Web Server` — for the custom `/history` GET handler

The build env (`sbse.ini`) adds `Mqtt`, `Mqtt Auto Discovery`, `Http`,
`WS`, `Wifi`, etc. as backend modules. They're not direct dependencies
of this module — the MQTT mirroring is automatic, and the HTTP/WS
stack is built by the `Web Server` module that this module *does*
depend on.

## Memory and code size

Approximate, last measured commit `e659a9ed2…`:

| Slice | Size |
|---|---|
| Total flash | ~1.69 MB / 25.7 % of partition |
| Total RAM (`.bss` + `.data`) | ~55.5 KB / 17 % of internal SRAM |
| `SbseTraceHistory` ring buffer | 3.6 KB |
| `TFModbusTCPServer` instance (incl. client slots) | ~3–4 KB |
| `Mqtt` + `Mqtt Auto Discovery` (incremental over no-MQTT build) | ~96 KB flash, ~3.5 KB RAM |

## What's *not* here

- **No tests.** There's no unit-test harness for this module. Sanity
  testing is "build, flash, watch the chart, send curl commands."
- **No automated dependency injection.** The control loop reaches the
  inverter through the `modbus_tcp_client` pool global; the trace
  history and Modbus server are owned by-value, so swapping
  implementations would be a code edit.
- **No simulator.** Every code path that writes to the inverter does so
  for real. There is no offline-test or dry-run mode.
