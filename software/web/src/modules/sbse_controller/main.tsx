/* esp32-firmware
 * Copyright (C) 2026 Thomas Hein
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

import * as util from "../../ts/util";
import * as API from "../../ts/api";
import { h, Fragment, Component, createRef, RefObject, ComponentChildren } from "preact";
import { __ } from "../../ts/translation";
import { Button } from "react-bootstrap";
import { Activity, AlertTriangle, Battery, BatteryCharging, Pause, Play, Sliders, Wifi, WifiOff, Zap } from "react-feather";

import { ConfigComponent } from "../../ts/components/config_component";
import { ConfigForm } from "../../ts/components/config_form";
import { FormRow } from "../../ts/components/form_row";
import { FormSeparator } from "../../ts/components/form_separator";
import { InputAnyFloat } from "../../ts/components/input_any_float";
import { InputHost } from "../../ts/components/input_host";
import { InputNumber } from "../../ts/components/input_number";
import { NavbarItem } from "../../ts/components/navbar_item";
import { StatusSection } from "../../ts/components/status_section";
import { SubPage } from "../../ts/components/sub_page";
import { Switch } from "../../ts/components/switch";
import { SwitchableInputNumber } from "../../ts/components/switchable_input_number";
import { UplotLoader } from "../../ts/components/uplot_loader";
import { UplotData, UplotPath, UplotWrapperB } from "../../ts/components/uplot_wrapper_2nd";
import { ModuleStatus, register_status_provider, StatusResult } from "../../ts/status_registry";

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

type Mode = "disabled" | "not_connected" | "stale" | "running" | "paused" | "safety" | "faulted";

const MODE_VARIANT: { [m: string]: string } = {
    running:       "success",
    stale:         "warning",
    paused:        "warning",
    safety:        "danger",
    faulted:       "danger",
    not_connected: "secondary",
    disabled:      "secondary",
};

function mode_label(mode: string): string {
    switch (mode) {
        case "disabled":      return __("sbse_controller.status.mode_disabled");
        case "not_connected": return __("sbse_controller.status.mode_not_connected");
        case "stale":         return __("sbse_controller.status.mode_stale");
        case "running":       return __("sbse_controller.status.mode_running");
        case "paused":        return __("sbse_controller.status.mode_paused");
        case "safety":        return __("sbse_controller.status.mode_safety");
        case "faulted":       return __("sbse_controller.status.mode_faulted");
        default:              return mode;
    }
}

function ModeBadge({mode}: {mode: string}) {
    const variant = MODE_VARIANT[mode] ?? "secondary";
    const icon = mode === "running" ? <Activity size={14}/>
               : mode === "safety" || mode === "faulted" ? <AlertTriangle size={14}/>
               : mode === "paused" ? <Pause size={14}/>
               : mode === "not_connected" ? <WifiOff size={14}/>
               : mode === "stale" ? <Wifi size={14}/>
               : <Activity size={14}/>;
    return (
        <span class={`badge bg-${variant} sbse-mode-pill`}>
            {icon}<span class="ms-1">{mode_label(mode)}</span>
        </span>
    );
}

function StatTile(props: {icon: any, label: string, value: string, unit?: string, accent?: string}) {
    return (
        <div class={`sbse-tile ${props.accent ? "sbse-tile-" + props.accent : ""}`}>
            <div class="sbse-tile-icon">{props.icon}</div>
            <div class="sbse-tile-body">
                <div class="sbse-tile-label">{props.label}</div>
                <div class="sbse-tile-value">
                    {props.value}
                    {props.unit ? <span class="sbse-tile-unit"> {props.unit}</span> : null}
                </div>
            </div>
        </div>
    );
}

function fmt_w(value: number): string {
    if (value === null || value === undefined || Number.isNaN(value)) return "—";
    const sign = value > 0 ? "+" : "";
    return sign + util.toLocaleFixed(value, 0);
}

function fmt_age_ms(ms: number): string {
    if (!ms || ms < 0) return "—";
    if (ms < 1000)   return `${ms} ms`;
    if (ms < 60_000) return `${util.toLocaleFixed(ms / 1000, 1)} s`;
    return `${util.toLocaleFixed(ms / 60_000, 1)} min`;
}

function LiveSliderRow(props: {
    label: string,
    help?: ComponentChildren,
    min: number,
    max: number,
    step: number,
    pending: number,
    current: number,
    onValue: (v: number) => void,
    onApply: () => void,
}) {
    return (
        <FormRow label={props.label} help={props.help}>
            <div class="d-flex gap-2 align-items-center">
                <input type="range"
                       class="form-range sbse-slider"
                       min={props.min} max={props.max} step={props.step}
                       value={props.pending}
                       onInput={(e) => props.onValue(parseInt((e.target as HTMLInputElement).value, 10))}/>
                <div style="min-width: 9em;">
                    <InputNumber unit="W"
                                 min={props.min} max={props.max}
                                 value={props.pending}
                                 onValue={props.onValue}/>
                </div>
                <Button variant="primary"
                        disabled={props.pending === props.current}
                        onClick={props.onApply}>
                    {__("sbse_controller.status.apply")}
                </Button>
            </div>
        </FormRow>
    );
}

// ---------------------------------------------------------------------------
// Navbar entry
// ---------------------------------------------------------------------------

export function SbseControllerNavbar() {
    return <NavbarItem name="sbse_controller" module="sbse_controller"
                       title={__("sbse_controller.navbar.title")}
                       symbol={<Sliders/>}/>;
}

// ---------------------------------------------------------------------------
// Status section (dashboard card)
// ---------------------------------------------------------------------------

// Live-tunable fields the dashboard exposes as slider+input+apply rows.
type LiveField = "target_grid_w" | "max_charge_w" | "max_discharge_w";

interface Pending {
    target_grid_w:   number;
    max_charge_w:    number;
    max_discharge_w: number;
}

interface SbseControllerStatusState {
    pending: Pending;              // pending values for the slider/input rows
    samples: Sample[];             // sliding-window buffer for the live trace
}

function snapshot_pending(): Pending {
    const ac = API.get("sbse_controller/active_config");
    return {
        target_grid_w:   ac.target_grid_w,
        max_charge_w:    ac.max_charge_w,
        max_discharge_w: ac.max_discharge_w,
    };
}

export class SbseControllerStatus extends Component<{}, SbseControllerStatusState> {
    constructor() {
        super();
        this.state = {
            pending: { target_grid_w: 0, max_charge_w: 5000, max_discharge_w: 5000 },
            samples: [],
        };

        util.addApiEventListener("sbse_controller/active_config", () => {
            this.setState({ pending: snapshot_pending() });
        });

        util.addApiEventListener("sbse_controller/state", () => {
            const st = API.get("sbse_controller/state");
            const ac = API.get("sbse_controller/active_config");
            const samples = [...this.state.samples];
            const now = Date.now() / 1000;

            samples.push({
                ts:       now,
                grid:     st.grid_w_ema,
                battery:  st.battery_w,
                setpoint: st.last_setpoint_w,
                target:   ac.target_grid_w,
            });

            const cutoff = now - CHART_WINDOW_S;
            while (samples.length > 0 && samples[0].ts < cutoff) {
                samples.shift();
            }
            this.setState({samples});
        });
    }

    set_pending = (field: LiveField, value: number) => {
        this.setState({ pending: { ...this.state.pending, [field]: value } });
    };

    apply_field = (field: LiveField, value: number) => {
        // target_grid_w is a setpoint (a wish), max_charge_w / max_discharge_w
        // are saturation limits. They are independent; the controller will
        // saturate at the limits when the target can't physically be reached.
        const next = { ...API.get("sbse_controller/active_config"), [field]: value };
        API.save("sbse_controller/active_config", next,
                 () => __("sbse_controller.script.save_active_failed"));
    };

    force_release = () => {
        API.call("sbse_controller/force_release", null,
                 () => __("sbse_controller.script.force_release_failed"));
    };

    resume = () => {
        API.call("sbse_controller/resume", null,
                 () => __("sbse_controller.script.resume_failed"));
    };

    render() {
        if (!util.render_allowed() || !API.hasModule("sbse_controller"))
            return <StatusSection name="sbse_controller"/>;

        const st  = API.get("sbse_controller/state");
        const ac  = API.get("sbse_controller/active_config");

        const charging  = st.battery_w < 0;
        const soc_unknown = st.battery_soc >= 255;

        return (
            <StatusSection name="sbse_controller">
                <div class="card sbse-card mb-3">
                    <div class="card-header d-flex justify-content-between align-items-center">
                        <span class="fw-bold">{__("sbse_controller.status.title")}</span>
                        <div class="d-flex align-items-center gap-2">
                            {st.simulation_mode ?
                                <span class="badge bg-info sbse-mode-pill">
                                    <AlertTriangle size={14}/>
                                    <span class="ms-1">{__("sbse_controller.status.sim_badge")}</span>
                                </span>
                            : null}
                            <ModeBadge mode={st.mode}/>
                        </div>
                    </div>
                    <div class="card-body">

                        <div class="sbse-tile-grid">
                            <StatTile icon={<Zap/>}
                                      label={__("sbse_controller.status.grid")}
                                      value={fmt_w(st.grid_w_ema)}
                                      unit="W"
                                      accent={st.grid_w_ema > 0 ? "import" : (st.grid_w_ema < 0 ? "export" : null)}/>
                            <StatTile icon={charging ? <BatteryCharging/> : <Battery/>}
                                      label={__("sbse_controller.status.battery")}
                                      value={fmt_w(st.battery_w)}
                                      unit="W"
                                      accent={charging ? "charge" : (st.battery_w > 0 ? "discharge" : null)}/>
                            <StatTile icon={<Battery/>}
                                      label={__("sbse_controller.status.soc")}
                                      value={soc_unknown ? "—" : `${st.battery_soc}`}
                                      unit="%"/>
                            <StatTile icon={<Activity/>}
                                      label={__("sbse_controller.status.setpoint")}
                                      value={fmt_w(st.last_setpoint_w)}
                                      unit="W"/>
                            <StatTile icon={<Activity/>}
                                      label={__("sbse_controller.status.last_write")}
                                      value={fmt_age_ms(st.last_write_age_ms)}/>
                            <StatTile icon={<AlertTriangle/>}
                                      label={__("sbse_controller.status.write_errors")}
                                      value={`${st.write_err_count}`}
                                      accent={st.write_err_count > 0 ? "danger" : null}/>
                        </div>

                        <hr/>

                        <SbseControllerChart samples={this.state.samples}/>

                        <hr/>

                        <LiveSliderRow
                            label={__("sbse_controller.status.target_grid_w")}
                            help={__("sbse_controller.status.target_grid_w_help")}
                            min={-10000}
                            max={10000}
                            step={50}
                            pending={this.state.pending.target_grid_w}
                            current={ac.target_grid_w}
                            onValue={(v) => this.set_pending("target_grid_w", v)}
                            onApply={() => this.apply_field("target_grid_w", this.state.pending.target_grid_w)}/>

                        <LiveSliderRow
                            label={__("sbse_controller.status.max_charge_w")}
                            help={__("sbse_controller.status.max_charge_w_help")}
                            min={0}
                            max={10000}
                            step={50}
                            pending={this.state.pending.max_charge_w}
                            current={ac.max_charge_w}
                            onValue={(v) => this.set_pending("max_charge_w", v)}
                            onApply={() => this.apply_field("max_charge_w", this.state.pending.max_charge_w)}/>

                        <LiveSliderRow
                            label={__("sbse_controller.status.max_discharge_w")}
                            help={__("sbse_controller.status.max_discharge_w_help")}
                            min={0}
                            max={10000}
                            step={50}
                            pending={this.state.pending.max_discharge_w}
                            current={ac.max_discharge_w}
                            onValue={(v) => this.set_pending("max_discharge_w", v)}
                            onApply={() => this.apply_field("max_discharge_w", this.state.pending.max_discharge_w)}/>

                        <div class="d-flex justify-content-end gap-2 mt-3">
                            <Button variant="outline-warning"
                                    disabled={st.mode === "paused"}
                                    onClick={this.force_release}>
                                <Pause size={16} class="me-1"/>
                                {__("sbse_controller.status.force_release")}
                            </Button>
                            <Button variant="outline-success"
                                    disabled={st.mode !== "paused"}
                                    onClick={this.resume}>
                                <Play size={16} class="me-1"/>
                                {__("sbse_controller.status.resume")}
                            </Button>
                        </div>

                        {st.last_error && st.last_error.length > 0 ?
                            <div class="alert alert-warning mt-3 mb-0 py-2">
                                <small><AlertTriangle size={14} class="me-1"/>{st.last_error}</small>
                            </div>
                        : null}
                    </div>
                </div>
            </StatusSection>
        );
    }
}

// ---------------------------------------------------------------------------
// Live trace chart (sliding window over the last CHART_WINDOW_S seconds)
// ---------------------------------------------------------------------------

const CHART_WINDOW_S = 5 * 60;

interface Sample {
    ts: number;            // unix-ish seconds (Date.now() / 1000)
    grid: number;          // W
    battery: number;       // W
    setpoint: number;      // W
    target: number;        // W
}

interface SbseControllerChartProps {
    samples: Sample[];
}

class SbseControllerChart extends Component<SbseControllerChartProps, {}> {
    uplot_loader_ref  = createRef<UplotLoader>();
    uplot_wrapper_ref = createRef<UplotWrapperB>();

    update_uplot = () => {
        const samples = this.props.samples;
        let data: UplotData;

        if (samples.length === 0) {
            data = { keys: [null], names: [null], values: [null] };
        } else {
            data = {
                keys:   [null,                       "grid",    "battery", "setpoint", "target"],
                names:  [null,
                         __("sbse_controller.chart.grid"),
                         __("sbse_controller.chart.battery"),
                         __("sbse_controller.chart.setpoint"),
                         __("sbse_controller.chart.target")],
                values: [[], [], [], [], []],
                paths:  [null, UplotPath.Line, UplotPath.Line, UplotPath.Line, UplotPath.Step],
            };
            for (const s of samples) {
                data.values[0].push(s.ts);
                data.values[1].push(s.grid);
                data.values[2].push(s.battery);
                data.values[3].push(s.setpoint);
                data.values[4].push(s.target);
            }
        }

        this.uplot_loader_ref.current?.set_data(samples.length > 1);
        this.uplot_wrapper_ref.current?.set_data(data);
    };

    override componentDidUpdate(prev: SbseControllerChartProps) {
        if (prev.samples !== this.props.samples) {
            this.update_uplot();
        }
    }

    render() {
        return (
            <div style="position: relative;">
                <UplotLoader ref={this.uplot_loader_ref}
                             show
                             marker_class="h4"
                             no_data={__("sbse_controller.chart.no_data")}
                             loading={__("sbse_controller.chart.loading")}>
                    <UplotWrapperB ref={this.uplot_wrapper_ref}
                                   class="sbse-chart"
                                   sub_page="status"
                                   color_cache_group="sbse_controller.default"
                                   show
                                   legend_show
                                   legend_time_label={__("sbse_controller.chart.time")}
                                   legend_time_with_minutes
                                   on_mount={this.update_uplot}
                                   aspect_ratio={3}
                                   x_format={{hour: "2-digit", minute: "2-digit", second: "2-digit"}}
                                   x_padding_factor={0}
                                   x_include_date={false}
                                   y_unit="W"
                                   y_label="W"
                                   y_digits={0}
                                   grid_show
                                   padding={[null, 15, null, 5]}
                                   height_min={180}/>
                </UplotLoader>
            </div>
        );
    }
}

// ---------------------------------------------------------------------------
// Settings sub-page
// ---------------------------------------------------------------------------

type ControllerConfig = API.getType["sbse_controller/config"];

export class SbseController extends ConfigComponent<"sbse_controller/config",
                                                    {status_ref?: RefObject<SbseControllerStatus>}> {
    constructor() {
        super("sbse_controller/config",
              () => __("sbse_controller.script.save_failed"));
    }

    render(props: {status_ref?: RefObject<SbseControllerStatus>},
           state: ControllerConfig) {
        if (!util.render_allowed())
            return <SubPage name="sbse_controller"/>;

        return (
            <SubPage name="sbse_controller">
                <ConfigForm id="sbse_controller_config_form"
                            title={__("sbse_controller.content.title")}
                            isDirty={this.isDirty()}
                            onSave={this.save}
                            onDirtyChange={this.setDirty}>

                    <FormSeparator heading={__("sbse_controller.content.section_connection")}/>

                    <FormRow label={__("sbse_controller.content.enabled")}
                             help={__("sbse_controller.content.enabled_help")}>
                        <Switch desc={__("sbse_controller.content.enabled_desc")}
                                checked={state.enabled}
                                onClick={this.toggle("enabled")}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.host")}
                             help={__("sbse_controller.content.host_help")}>
                        <InputHost value={state.host} onValue={this.set("host")}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.port")}>
                        <InputNumber min={1} max={65535} value={state.port} onValue={this.set("port")}/>
                    </FormRow>

                    <FormSeparator heading={__("sbse_controller.content.section_mode")}/>

                    <FormRow label={__("sbse_controller.content.simulation_mode")}
                             help={__("sbse_controller.content.simulation_mode_help")}>
                        <Switch desc={__("sbse_controller.content.simulation_mode_desc")}
                                checked={state.simulation_mode}
                                onClick={this.toggle("simulation_mode")}/>
                    </FormRow>

                    <FormSeparator heading={__("sbse_controller.content.section_timing")}/>

                    <FormRow label={__("sbse_controller.content.tick_ms")}
                             help={__("sbse_controller.content.tick_ms_help")}>
                        <InputNumber min={50} max={5000} unit="ms"
                                     value={state.tick_ms} onValue={this.set("tick_ms")}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.soc_interval_ms")}
                             help={__("sbse_controller.content.soc_interval_ms_help")}>
                        <InputNumber min={100} max={60000} unit="ms"
                                     value={state.soc_interval_ms} onValue={this.set("soc_interval_ms")}/>
                    </FormRow>

                    <FormSeparator heading={__("sbse_controller.content.section_targets")}/>

                    <FormRow label={__("sbse_controller.content.target_grid_w")}
                             help={__("sbse_controller.content.target_grid_w_help")}>
                        <InputNumber min={-10000} max={10000} unit="W"
                                     value={state.target_grid_w}
                                     onValue={this.set("target_grid_w")}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.max_charge_w")}
                             help={__("sbse_controller.content.max_charge_w_help")}>
                        <InputNumber min={0} max={10000} unit="W"
                                     value={state.max_charge_w}
                                     onValue={this.set("max_charge_w")}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.max_discharge_w")}
                             help={__("sbse_controller.content.max_discharge_w_help")}>
                        <InputNumber min={0} max={10000} unit="W"
                                     value={state.max_discharge_w}
                                     onValue={this.set("max_discharge_w")}/>
                    </FormRow>

                    <FormSeparator heading={__("sbse_controller.content.section_tuning")}/>

                    <FormRow label={__("sbse_controller.content.kp")}
                             help={__("sbse_controller.content.kp_help")}>
                        <InputAnyFloat min={0.10} max={2.00}
                                       value={state.kp_milli / 1000}
                                       onValue={(v) => this.setState({kp_milli: Math.round(v * 1000)})}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.kd")}
                             help={__("sbse_controller.content.kd_help")}>
                        <InputAnyFloat min={0.00} max={3.00}
                                       value={state.kd_milli / 1000}
                                       onValue={(v) => this.setState({kd_milli: Math.round(v * 1000)})}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.alpha_grid")}
                             help={__("sbse_controller.content.alpha_grid_help")}>
                        <InputAnyFloat min={0.01} max={1.00}
                                       value={state.alpha_grid_milli / 1000}
                                       onValue={(v) => this.setState({alpha_grid_milli: Math.round(v * 1000)})}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.alpha_setpoint")}
                             help={__("sbse_controller.content.alpha_setpoint_help")}>
                        <InputAnyFloat min={0.01} max={1.00}
                                       value={state.alpha_setpoint_milli / 1000}
                                       onValue={(v) => this.setState({alpha_setpoint_milli: Math.round(v * 1000)})}/>
                    </FormRow>

                    <FormRow label={__("sbse_controller.content.deadband_w")}
                             help={__("sbse_controller.content.deadband_w_help")}>
                        <InputNumber min={0} max={1000} unit="W"
                                     value={state.deadband_w}
                                     onValue={this.set("deadband_w")}/>
                    </FormRow>

                    <FormSeparator heading={__("sbse_controller.content.section_safety")}/>

                    <FormRow label={__("sbse_controller.content.safety_zero_after_failures")}
                             help={__("sbse_controller.content.safety_zero_after_failures_help")}>
                        <SwitchableInputNumber
                            switch_label_active={__("sbse_controller.content.enabled_label")}
                            switch_label_inactive={__("sbse_controller.content.disabled_label")}
                            checked={state.safety_zero_after_failures > 0}
                            onClick={() => this.setState({
                                safety_zero_after_failures: state.safety_zero_after_failures > 0 ? 0 : 5,
                            })}
                            value={state.safety_zero_after_failures}
                            onValue={(v) => this.setState({safety_zero_after_failures: v})}
                            min={1}
                            max={100}
                            switch_label_min_width="110px"/>
                    </FormRow>

                </ConfigForm>
            </SubPage>
        );
    }
}

// ---------------------------------------------------------------------------
// Status provider (top-right header badge)
// ---------------------------------------------------------------------------

function build_status(): StatusResult | null {
    if (!API.hasModule("sbse_controller")) {
        return null;
    }

    const st = API.get("sbse_controller/state");
    const ac = API.get("sbse_controller/active_config");

    if (!st || !ac) {
        return null;
    }

    const sim_suffix = st.simulation_mode ? ` (${__("sbse_controller.status.sim_badge")})` : "";

    switch (st.mode) {
        case "disabled":
            return {
                status: ModuleStatus.Disabled,
                text:   () => mode_label(st.mode) + sim_suffix,
            };
        case "running":
            return {
                status: st.simulation_mode ? ModuleStatus.Warning : ModuleStatus.Ok,
                text:   () => `${fmt_w(st.grid_w_ema)} W → ${fmt_w(ac.target_grid_w)} W` + sim_suffix,
            };
        case "stale":
        case "paused":
        case "not_connected":
            return {
                status: ModuleStatus.Warning,
                text:   () => (st.last_error && st.last_error.length > 0
                              ? st.last_error
                              : mode_label(st.mode)) + sim_suffix,
            };
        case "safety":
        case "faulted":
            return {
                status: ModuleStatus.Error,
                text:   () => (st.last_error && st.last_error.length > 0
                              ? st.last_error
                              : mode_label(st.mode)) + sim_suffix,
            };
        default:
            return {
                status: ModuleStatus.Warning,
                text:   () => mode_label(st.mode) + sim_suffix,
            };
    }
}

export function pre_init() {}

export function init() {
    register_status_provider("sbse_controller", {
        name: () => __("sbse_controller.navbar.title"),
        href: "#sbse_controller",
        get_status: build_status,
    });
}
