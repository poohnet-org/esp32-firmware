export interface config {
    enabled: boolean;
    host: string;
    port: number;
    tick_ms: number;
    soc_interval_ms: number;
    target_grid_w: number;
    max_charge_w: number;
    max_discharge_w: number;
    kp_milli: number;
    alpha_grid_milli: number;
    alpha_setpoint_milli: number;
    deadband_w: number;
    safety_zero_after_failures: number;
    simulation_mode: boolean;
}

export interface active_config {
    target_grid_w: number;
    max_charge_w: number;
    max_discharge_w: number;
    kp_milli: number;
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
    last_error: string;
}

export interface force_release {
}
