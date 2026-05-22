/** @jsxImportSource preact */
import { h } from "preact";
let x = {
    "sbse_controller": {
        "navbar": {
            "title": "SBSE Controller"
        },
        "status": {
            "title": "SBSE Controller",
            "grid": "Grid",
            "battery": "Battery",
            "soc": "State of charge",
            "setpoint": "Setpoint",
            "last_write": "Last write",
            "write_errors": "Write errors",
            "grid_target_range":      "Grid target range",
            "grid_target_range_help": <>The <code>[lower, upper]</code> grid deadzone. The left handle is <code>grid_charge_target_w</code> (controller charges the battery to keep the grid at or above this). The right handle is <code>grid_discharge_target_w</code> (controller discharges to keep the grid at or below this). Slide them to the <em>same</em> value for hard single-target chase, or apart for a soft deadzone in which the battery stays idle between the two values. Live update, not persisted to flash.</>,
            "max_charge_w":  "Max charge power",
            "max_charge_w_help":  <>Upper bound on battery charging power. Live update, not persisted to flash. <code>0</code> disables charging.</>,
            "max_discharge_w": "Max discharge power",
            "max_discharge_w_help": <>Upper bound on battery discharging power. Live update, not persisted to flash. <code>0</code> disables discharging.</>,
            "apply": "Apply",
            "pause":  "Pause 30 s (0 W)",
            "resume": "Resume",
            "mb_badge":  "MB",
            "mb_badge_help_title": "An external Modbus TCP client is currently steering the controller",
            "read_led_help_title": "Flashes whenever the SMA-hybrid proxy serves a Modbus read",
            "write_led_help_title": "Flashes whenever an external client writes (OpMod or setpoint) over Modbus",
            "hard_badge": "HARD",
            "hard_badge_help_title": "Hard target: charge and discharge targets are equal; the controller chases that single grid value in both directions.",
            "soft_badge": "SOFT",
            "soft_badge_help_title": "Soft target: charge and discharge targets differ. The battery is idle inside the [charge, discharge] grid deadzone; outside, the controller chases the nearer bound.",

            "mode_disabled":        "disabled",
            "mode_not_connected":   "not connected",
            "mode_stale":           "stale",
            "mode_running":         "running",
            "mode_paused":          "paused",
            "mode_safety":          "safety",
            "mode_faulted":         "faulted",
            "mode_force_charge":    "force charge",
            "mode_force_discharge": "force discharge",
            "mode_blocked":         "blocked",
            "mode_block_charge":    "charge blocked",
            "mode_block_discharge": "discharge blocked"
        },
        "chart": {
            "grid":      "Grid (EMA)",
            "battery":   "Battery",
            "setpoint":  "Setpoint",
            "target_lo": "Charge target",
            "target_hi": "Discharge target",
            "time":      "Time",
            "no_data":   "No samples yet",
            "loading":   "Collecting samples…"
        },
        "content": {
            "title": "SBSE Controller",

            "section_connection": "Connection",
            "section_timing":     "Loop timing",
            "section_targets":    "Control targets",
            "section_tuning":     "Controller tuning",
            "section_safety":     "Safety",

            "enabled":         "Enabled",
            "enabled_desc":    "Run the setpoint controller",
            "enabled_help":    <>Master switch. Takes effect on next reboot (the network-connect event that starts the TCP session has already been delivered).</>,
            "enabled_label":   "enabled",
            "disabled_label":  "disabled",

            "host":      "Inverter host",
            "host_help": <>IP address or hostname of the SBSE Modbus TCP gateway. A change requires a reboot to take effect.</>,
            "port":      "Modbus TCP port",
            "port_help": <>TCP port of the SBSE Modbus gateway. A change requires a reboot to take effect.</>,

            "tick_ms":      "Tick interval",
            "tick_ms_help": <>Control loop period. One read–compute–write cycle per tick. Lower = faster grid tracking; higher = less Modbus traffic. A change requires a reboot to take effect.</>,

            "soc_interval_ms":      "SoC poll interval",
            "soc_interval_ms_help": <>How often to read the battery SoC. SoC changes slowly; polling it every tick is wasted bandwidth. Must be ≥ tick interval. A change requires a reboot to take effect.</>,

            "grid_charge_target_w":      "Charge target (lower grid bound)",
            "grid_charge_target_w_help": <>Lower bound of the grid deadzone. When the grid would go below this value (e.g. PV is producing surplus beyond this export setpoint), the controller charges the battery to bring it back up. Set equal to the discharge target for hard single-target chase; set lower for an asymmetric deadzone (soft mode). Typical value for self-consumption: <code>0 W</code>; for "let me export up to 200 W before the battery starts charging": <code>-200 W</code>.</>,

            "grid_discharge_target_w":      "Discharge target (upper grid bound)",
            "grid_discharge_target_w_help": <>Upper bound of the grid deadzone. When the grid would go above this value (i.e. importing), the controller discharges the battery to bring it back down. Must be ≥ charge target. Typical values: <code>0 W</code> (no autonomous grid import; battery covers any deficit) for self-consumption with a no-import preference; the same value as the charge target for hard-target chase; any larger value to allow some import before discharging kicks in.</>,

            "max_charge_w":      "Max charge power",
            "max_charge_w_help": <>Upper bound on battery charging power. 0 forbids charging entirely.</>,

            "max_discharge_w":      "Max discharge power",
            "max_discharge_w_help": <>Upper bound on battery discharging power. 0 forbids discharge.</>,

            "kp":      "P-gain (Kp)",
            "kp_help": <>Proportional gain of the controller. <code>new_setpoint = battery_now + Kp · (ema_grid − target) + Kd · Δema_grid</code>. Lower = slower, more damped. Higher = snappier but can ring.</>,

            "kd":      "D-gain (Kd)",
            "kd_help": <>Derivative gain, computed on the smoothed grid measurement (not on the error, so changes to the target don't kick the output). <code>0</code> disables the D term entirely. Raise it if fast load steps (induction plate, kettle) cause the P-controller to overshoot before the grid EMA catches up. As a rule of thumb, start at <code>1.0</code>; increase toward <code>2.0</code>–<code>3.0</code> if overshoot persists; reduce if the chart starts oscillating.</>,

            "alpha_grid":      "Grid input smoothing (α)",
            "alpha_grid_help": <>EMA factor on the grid-power measurement. <code>α=1.0</code> disables smoothing; lower values filter more aggressively at the cost of lag.</>,

            "alpha_setpoint":      "Setpoint smoothing (α)",
            "alpha_setpoint_help": <>EMA factor on the commanded battery setpoint. Smooths the output to the inverter to keep noisy grid readings from producing flickery commands.</>,

            "deadband_w":      "Write deadband",
            "deadband_w_help": <>If the new computed setpoint is within ±deadband of the last commanded value, the Modbus write is skipped. The SBSE keeps holding the last value, so generous deadbands are safe.</>,

            "safety_zero_after_failures":      "Safety-zero after N failed reads",
            "safety_zero_after_failures_help": <>After this many consecutive failed read cycles, the controller commands a one-shot 0 W setpoint and enters the safety mode until reads recover. Disable to leave the last setpoint in place forever during outages.</>,

            "keepalive_interval_s":      "Keep-alive / write-watchdog interval",
            "keepalive_interval_s_help": <>Caps the silent-write gap so the inverter sees periodic Modbus activity in two complementary ways: <strong>(1) idle pulse</strong> — when the battery has been idle this long, the controller emits a small alternating ±<code>keepalive_pulse_w</code> pulse to prevent the SBSE's ~10–15 min low-power standby (which costs ~20–30 s on the next wake-up); <strong>(2) active refresh</strong> — when the battery is active but the setpoint sits inside the write deadband for longer than the interval (steady operation, force-mode at a constant value), the controller re-asserts the current setpoint anyway. Default 480 s = 8 min stays comfortably below the standby threshold. Disable to let the inverter standby normally — useful if you want lower idle losses and don't mind the wake-up lag.</>,

            "keepalive_pulse_w":      "Keep-alive pulse magnitude",
            "keepalive_pulse_w_help": <>How big each keep-alive pulse is. The pulse alternates sign between successive fires so the long-run energy contribution averages to zero. Each pulse lasts exactly one tick (typ. 300 ms), so the actual energy moved is in the millijoule range — well under a cell's noise floor. Default <code>50 W</code> is large enough to register as "battery active" on the inverter but small enough to be invisible on the grid. Set to <code>0</code> to disable keep-alive without touching the interval setting.</>,

            "section_modbus_server":           "Modbus TCP server (external control)",
            "modbus_server_enabled":           "Modbus TCP server",
            "modbus_server_enabled_desc":      "Accept external SMA-compatible setpoint writes",
            "modbus_server_enabled_help":      <>Starts a Modbus TCP server that accepts the same WriteMultipleRegisters commands that the WARP charger's "SMA Hybrid Inverter" battery class sends to a real Sunny Boy Storage. Block, Normal, Block Discharge, Block Charge, Force Charge and Force Discharge are all supported. Each write also updates the live <code>active_config</code>, so the dashboard reflects what the external client is doing. Operator changes via the dashboard / HTTP / MQTT take over instantly ("last write wins"). Requires a reboot if the port changes.</>,
            "modbus_server_port":              "Port",
            "modbus_server_port_help":         <>TCP port to listen on. SMA's default is <code>502</code>. A change requires a reboot to take effect.</>,
            "modbus_server_unit_id":           "Unit ID",
            "modbus_server_unit_id_help":      <>Modbus unit id this server responds to. Default <code>3</code> matches the SMA Hybrid Inverter convention so existing clients work unchanged. Set to <code>0</code> to accept any unit id.</>,
            "modbus_server_watchdog_s":        "Watchdog timeout",
            "modbus_server_watchdog_s_help":   <>If no Modbus write arrives within this many seconds, the controller reverts the live overrides (<code>target_grid_w</code>, <code>max_charge_w</code>, <code>max_discharge_w</code>) to the persistent config values and exits force-mode. SMA's real-inverter watchdog is 5 minutes; the default of 60 s matches the resend interval that the WARP charger uses. Disable to leave the most recent setpoint in place forever.</>,
            "modbus_server_authority":            "Modbus authority",
            "modbus_server_authority_help":       <>How much of the operator's <code>active_config</code> an external Modbus client is allowed to overwrite on every WriteMultipleRegisters at register 40793. The SMA <code>OpMod</code> force-mode commands (2289 / 2290) are always honoured; this setting only governs the persistent caps and grid targets.</>,
            "modbus_server_authority_force_only": "Force commands only — preserve all my settings",
            "modbus_server_authority_caps":       "Caps — apply max_charge_w / max_discharge_w (default)",
            "modbus_server_authority_full":       "Full — also apply GridWSpt to both grid targets"
        },
        "script": {
            "save_failed":          "Failed to save the SBSE controller settings",
            "save_active_failed":   "Failed to apply the live override",
            "pause_failed":        "Failed to pause the controller",
            "resume_failed":        "Failed to resume the controller"
        }
    }
}
