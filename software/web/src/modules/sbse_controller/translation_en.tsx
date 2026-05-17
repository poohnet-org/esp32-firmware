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
            "grid_charge_target_w": "Charge target (lower grid bound)",
            "grid_charge_target_w_help": <>If the grid would go below this value, the controller charges the battery to bring it back up. Live update, not persisted to flash. Set equal to the discharge target for a hard single-target chase; set lower (e.g. <code>-200 W</code>) than the discharge target for an asymmetric deadzone in which the battery is idle between the two bounds.</>,
            "grid_discharge_target_w": "Discharge target (upper grid bound)",
            "grid_discharge_target_w_help": <>If the grid would go above this value, the controller discharges the battery to bring it back down. Live update, not persisted to flash. Must be ≥ charge target. Typical values: <code>0 W</code> (no autonomous grid import) for self-consumption, or any larger value to allow import up to that point before discharging.</>,
            "max_charge_w":  "Max charge power",
            "max_charge_w_help":  <>Upper bound on battery charging power. Live update, not persisted to flash. <code>0</code> disables charging.</>,
            "max_discharge_w": "Max discharge power",
            "max_discharge_w_help": <>Upper bound on battery discharging power. Live update, not persisted to flash. <code>0</code> disables discharging.</>,
            "apply": "Apply",
            "force_release": "Pause 30 s (0 W)",
            "resume":        "Resume",
            "sim_badge": "SIM",
            "mb_badge":  "MB",
            "mb_badge_help_title": "An external Modbus TCP client is currently steering the controller",
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
            "modbus_server_use_grid_spt":      "Use Modbus GridWSpt for target_grid_w",
            "modbus_server_use_grid_spt_desc": "Let Modbus writes overwrite the configured target",
            "modbus_server_use_grid_spt_help": <>When <strong>off</strong> (default), Modbus writes to register 40793 only update <code>max_charge_w</code> and <code>max_discharge_w</code>; the operator-configured <code>target_grid_w</code> is preserved. This matches the WARP charger's "SMA Hybrid Inverter" battery class, which always sends <code>GridWSpt = 0</code> regardless of mode -- enabling this option there would zero out your target on every mode change. When <strong>on</strong>, every Modbus write also sets <code>active_config.target_grid_w</code> from the <code>GridWSpt</code> field. Enable for clients that genuinely use the grid-setpoint field.</>
        },
        "script": {
            "save_failed":          "Failed to save the SBSE controller settings",
            "save_active_failed":   "Failed to apply the live override",
            "force_release_failed": "Failed to trigger force_release",
            "resume_failed":        "Failed to resume the controller"
        }
    }
}
