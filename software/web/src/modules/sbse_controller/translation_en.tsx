/** @jsxImportSource preact */
import { h } from "preact";
let x = {
    "sbse_controller": {
        "navbar": {
            "title": "SBSE Controller"
        },
        "status": {
            "title": "SBSE 5.0 Controller",
            "grid": "Grid",
            "battery": "Battery",
            "soc": "State of charge",
            "setpoint": "Setpoint",
            "last_write": "Last write",
            "write_errors": "Write errors",
            "target_grid_w": "Target net grid",
            "target_grid_w_help": <>Positive = import from grid. Negative = export to grid. Live update, not persisted to flash.</>,
            "apply": "Apply",
            "force_release": "Pause 30 s (0 W)",
            "sim_badge": "SIM",

            "mode_disabled":      "disabled",
            "mode_not_connected": "not connected",
            "mode_stale":         "stale",
            "mode_running":       "running",
            "mode_paused":        "paused",
            "mode_safety":        "safety",
            "mode_faulted":       "faulted"
        },
        "chart": {
            "grid":     "Grid (EMA)",
            "battery":  "Battery",
            "setpoint": "Setpoint",
            "target":   "Target",
            "time":     "Time",
            "no_data":  "No samples yet",
            "loading":  "Collecting samples…"
        },
        "content": {
            "title": "SBSE 5.0 Controller",

            "section_connection": "Connection",
            "section_mode":       "Operating mode",
            "section_timing":     "Loop timing",
            "section_targets":    "Control targets",
            "section_tuning":     "Controller tuning",
            "section_safety":     "Safety",

            "simulation_mode":      "Simulation mode",
            "simulation_mode_desc": "Run the controller without writing to the inverter",
            "simulation_mode_help": <>When enabled, the controller still reads grid/battery/SoC and computes setpoints exactly as in normal operation, but skips the actual Modbus write. Useful for verifying tuning before letting it touch the battery. The dashboard chart, the read-only state, and the deadband logic all behave as if the writes had gone out.</>,

            "enabled":         "Enabled",
            "enabled_desc":    "Run the setpoint controller",
            "enabled_help":    <>Master switch. Takes effect on next reboot (the network-connect event that starts the TCP session has already been delivered).</>,
            "enabled_label":   "enabled",
            "disabled_label":  "disabled",

            "host":      "Inverter host",
            "host_help": <>IP address or hostname of the SBSE Modbus TCP gateway.</>,
            "port":      "Modbus TCP port",

            "tick_ms":      "Tick interval",
            "tick_ms_help": <>Control loop period. One read–compute–write cycle per tick. Lower = faster grid tracking; higher = less Modbus traffic.</>,

            "soc_interval_ms":      "SoC poll interval",
            "soc_interval_ms_help": <>How often to read the battery SoC. SoC changes slowly; polling it every tick is wasted bandwidth. Must be ≥ tick interval.</>,

            "target_grid_w":      "Target net grid power",
            "target_grid_w_help": <>The grid power the controller drives toward. Positive = import from grid; negative = export. 0 = pure self-consumption.</>,

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
            "safety_zero_after_failures_help": <>After this many consecutive failed read cycles, the controller commands a one-shot 0 W setpoint and enters the safety mode until reads recover. Disable to leave the last setpoint in place forever during outages.</>
        },
        "script": {
            "save_failed":          "Failed to save the SBSE controller settings",
            "save_active_failed":   "Failed to apply the live override",
            "force_release_failed": "Failed to trigger force_release"
        }
    }
}
