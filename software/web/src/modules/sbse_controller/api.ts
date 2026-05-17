export interface config {
    enabled: boolean;
    host: string;
    port: number;
    tick_ms: number;
    soc_interval_ms: number;
    grid_charge_target_w: number;
    grid_discharge_target_w: number;
    max_charge_w: number;
    max_discharge_w: number;
    kp_milli: number;
    kd_milli: number;
    alpha_grid_milli: number;
    alpha_setpoint_milli: number;
    deadband_w: number;
    safety_zero_after_failures: number;
    simulation_mode: boolean;
    modbus_server_enabled: boolean;
    modbus_server_port: number;
    modbus_server_unit_id: number;
    modbus_server_watchdog_s: number;
    modbus_server_use_grid_spt: boolean;
}

export interface active_config {
    grid_charge_target_w: number;
    grid_discharge_target_w: number;
    max_charge_w: number;
    max_discharge_w: number;
    kp_milli: number;
    kd_milli: number;
    alpha_grid_milli: number;
    alpha_setpoint_milli: number;
    deadband_w: number;
    safety_zero_after_failures: number;
    simulation_mode: boolean;
}

export interface state {
    mode: string;
    last_setpoint_w: number;
    last_write_age_ms: number;
    grid_w_raw: number;
    grid_w_ema: number;
    battery_w: number;
    battery_soc: number;
    write_ok_count: number;
    write_err_count: number;
    read_fail_streak: number;
    simulation_mode: boolean;
    modbus_active: boolean;
    modbus_op_mod: number;
    modbus_force_w: number;
    last_error: string;
}

export interface force_release {
}

export interface resume {
}
