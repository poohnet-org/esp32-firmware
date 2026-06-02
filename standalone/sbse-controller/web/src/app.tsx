import { render } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import uPlot from "uplot";

// ---- API types (mirror the Go control.State / config structs) ----

interface State {
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
  modbus_active: boolean;
  modbus_op_mod: number;
  modbus_force_w: number;
  modbus_read_count: number;
  modbus_write_count: number;
  last_error: string;
}

interface ActiveConfig {
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
  keepalive_interval_s: number;
  keepalive_pulse_w: number;
}

interface Config extends ActiveConfig {
  enabled: boolean;
  host: string;
  port: number;
  tick_ms: number;
  soc_interval_ms: number;
  modbus_server_enabled: boolean;
  modbus_server_port: number;
  modbus_server_unit_id: number;
  modbus_server_watchdog_s: number;
  modbus_server_authority: number;
}

// ---- mode pill labels + colours (mirror main.tsx MODE_VARIANT) ----

const MODE_LABEL: Record<string, string> = {
  disabled: "Disabled", not_connected: "Not connected", stale: "Stale",
  running: "Running", faulted: "Faulted", paused: "Paused", safety: "Safety",
  force_charge: "Force charge", force_discharge: "Force discharge",
  blocked: "Blocked", block_charge: "Block charge", block_discharge: "Block discharge",
};
const MODE_CLASS: Record<string, string> = {
  running: "ok", force_charge: "info", force_discharge: "info",
  paused: "warn", stale: "warn", block_charge: "warn", block_discharge: "warn", blocked: "warn",
  faulted: "err", safety: "err", not_connected: "muted", disabled: "muted",
};

const CHART_WINDOW_S = 5 * 60;

const get = (url: string) => fetch(url).then((r) => r.json());
const put = (url: string, body: unknown) =>
  fetch(url, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
const post = (url: string) => fetch(url, { method: "POST" });

function fmtW(v: number): string {
  if (Math.abs(v) >= 1000) return (v / 1000).toFixed(2) + " kW";
  return v + " W";
}

// ---- small presentational components ----

function ModePill({ mode }: { mode: string }) {
  return <span class={"pill pill-" + (MODE_CLASS[mode] ?? "muted")}>{MODE_LABEL[mode] ?? mode}</span>;
}

function SoftHardBadge({ lo, hi }: { lo: number; hi: number }) {
  return <span class={"pill " + (lo === hi ? "pill-hard" : "pill-soft")}>{lo === hi ? "HARD" : "SOFT"}</span>;
}

// ActivityLED flashes when count changes.
function ActivityLED({ count, label }: { count: number; label: string }) {
  const [on, setOn] = useState(false);
  const prev = useRef(count);
  useEffect(() => {
    if (count !== prev.current) {
      prev.current = count;
      setOn(true);
      const id = setTimeout(() => setOn(false), 150);
      return () => clearTimeout(id);
    }
  }, [count]);
  return <span class="led-wrap"><span class={"led " + (on ? "led-on" : "")} /> {label}</span>;
}

function Tile({ label, value, accent }: { label: string; value: string; accent?: string }) {
  return (
    <div class={"tile " + (accent ? "tile-" + accent : "")}>
      <div class="tile-label">{label}</div>
      <div class="tile-value">{value}</div>
    </div>
  );
}

// ---- live chart (uPlot) ----

type Row = [number, number, number, number, number, number]; // t, grid, battery, setpoint, lo, hi

function Chart({ rows }: { rows: Row[] }) {
  const el = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);

  useEffect(() => {
    if (!el.current) return;
    const opts: uPlot.Options = {
      width: el.current.clientWidth || 800,
      height: 320,
      scales: { x: { time: true } },
      series: [
        {},
        { label: "Grid", stroke: "#0d6efd", width: 2 },
        { label: "Battery", stroke: "#198754", width: 2 },
        { label: "Setpoint", stroke: "#fd7e14", width: 1, dash: [6, 3] },
        { label: "Lo", stroke: "#6c757d", width: 1, dash: [2, 2] },
        { label: "Hi", stroke: "#adb5bd", width: 1, dash: [2, 2] },
      ],
      axes: [{}, { label: "W" }],
      legend: { live: true },
    };
    plot.current = new uPlot(opts, transpose(rows), el.current);
    const onResize = () => plot.current?.setSize({ width: el.current!.clientWidth, height: 320 });
    window.addEventListener("resize", onResize);
    return () => { window.removeEventListener("resize", onResize); plot.current?.destroy(); plot.current = null; };
  }, []);

  useEffect(() => { plot.current?.setData(transpose(rows)); }, [rows]);

  return <div ref={el} class="chart" />;
}

function transpose(rows: Row[]): uPlot.AlignedData {
  const cols: number[][] = [[], [], [], [], [], []];
  for (const r of rows) for (let i = 0; i < 6; i++) cols[i].push(r[i]);
  return cols as uPlot.AlignedData;
}

// ---- editable number row ----

function NumRow({ label, value, onChange, min, max, unit }: {
  label: string; value: number; onChange: (v: number) => void;
  min?: number; max?: number; unit?: string;
}) {
  return (
    <label class="row">
      <span class="row-label">{label}</span>
      <input type="number" value={value} min={min} max={max}
        onInput={(e) => onChange(Number((e.target as HTMLInputElement).value))} />
      {unit ? <span class="row-unit">{unit}</span> : null}
    </label>
  );
}

// ---- main app ----

function App() {
  const [state, setState] = useState<State | null>(null);
  const [active, setActive] = useState<ActiveConfig | null>(null);
  const [config, setConfig] = useState<Config | null>(null);
  const [rows, setRows] = useState<Row[]>([]);
  const activeRef = useRef<ActiveConfig | null>(null);
  activeRef.current = active;

  // Seed chart from history, then keep live via SSE.
  useEffect(() => {
    get("/sbse_controller/active_config").then(setActive);
    get("/sbse_controller/config").then(setConfig);

    get("/sbse_controller/history").then((h: { samples: Row[] }) => {
      const now = Date.now() / 1000;
      // history rows are [age_ms, grid, battery, setpoint, lo, hi]
      const seeded: Row[] = (h.samples || []).map((s: any) => [now - s[0] / 1000, s[1], s[2], s[3], s[4], s[5]]);
      setRows(seeded);
    });

    const es = new EventSource("/sbse_controller/state/sse");
    es.addEventListener("state", (e) => {
      const st: State = JSON.parse((e as MessageEvent).data);
      setState(st);
      const a = activeRef.current;
      const lo = a ? a.grid_charge_target_w : 0;
      const hi = a ? a.grid_discharge_target_w : 0;
      const t = Date.now() / 1000;
      setRows((prev) => {
        const next = [...prev, [t, st.grid_w_ema, st.battery_w, st.last_setpoint_w, lo, hi] as Row];
        const cutoff = t - CHART_WINDOW_S;
        return next.filter((r) => r[0] >= cutoff);
      });
    });
    es.addEventListener("active_config", (e) => setActive(JSON.parse((e as MessageEvent).data)));
    es.addEventListener("config", (e) => setConfig(JSON.parse((e as MessageEvent).data)));
    return () => es.close();
  }, []);

  return (
    <div class="wrap">
      <header>
        <h1>SBSE Controller</h1>
        <div class="header-status">
          {state ? <ModePill mode={state.mode} /> : <span class="pill pill-muted">…</span>}
          {active ? <SoftHardBadge lo={active.grid_charge_target_w} hi={active.grid_discharge_target_w} /> : null}
          {state ? <ActivityLED count={state.modbus_read_count} label="MB read" /> : null}
          {state ? <ActivityLED count={state.modbus_write_count} label="MB write" /> : null}
          {state?.modbus_active ? <span class="pill pill-info">MB</span> : null}
        </div>
      </header>

      {state?.last_error ? <div class="banner err">{state.last_error}</div> : null}

      <section class="card">
        <div class="tiles">
          <Tile label="Grid" value={state ? fmtW(state.grid_w_ema) : "—"} accent="grid" />
          <Tile label="Battery" value={state ? fmtW(state.battery_w) : "—"} accent={state && state.battery_w < 0 ? "charge" : "discharge"} />
          <Tile label="Setpoint" value={state ? fmtW(state.last_setpoint_w) : "—"} />
          <Tile label="SoC" value={state && state.battery_soc !== 255 ? state.battery_soc + " %" : "—"} />
        </div>
        <Chart rows={rows} />
        <div class="commands">
          <button onClick={() => post("/sbse_controller/pause")}>Pause 30 s</button>
          <button onClick={() => post("/sbse_controller/resume")}>Resume</button>
        </div>
      </section>

      {active ? <ActiveCard active={active} /> : null}
      {config ? <ConfigCard config={config} /> : null}

      <footer>
        {state ? (
          <span>
            writes ok {state.write_ok_count} / err {state.write_err_count} · read-fail streak {state.read_fail_streak} ·
            last write {Math.round(state.last_write_age_ms / 1000)} s ago
          </span>
        ) : null}
      </footer>
    </div>
  );
}

function ActiveCard({ active }: { active: ActiveConfig }) {
  const [d, setD] = useState<ActiveConfig>(active);
  const dirty = JSON.stringify(d) !== JSON.stringify(active);
  useEffect(() => { if (!dirty) setD(active); }, [active]); // adopt server updates unless editing
  const f = (k: keyof ActiveConfig) => (v: number) => setD({ ...d, [k]: v });
  return (
    <section class="card">
      <h2>Live tuning <small>(active_config)</small></h2>
      <div class="grid2">
        <NumRow label="Grid charge target (lo)" value={d.grid_charge_target_w} onChange={f("grid_charge_target_w")} min={-750} max={2500} unit="W" />
        <NumRow label="Grid discharge target (hi)" value={d.grid_discharge_target_w} onChange={f("grid_discharge_target_w")} min={-750} max={2500} unit="W" />
        <NumRow label="Max charge" value={d.max_charge_w} onChange={f("max_charge_w")} min={0} max={10000} unit="W" />
        <NumRow label="Max discharge" value={d.max_discharge_w} onChange={f("max_discharge_w")} min={0} max={10000} unit="W" />
        <NumRow label="Kp (×1000)" value={d.kp_milli} onChange={f("kp_milli")} min={100} max={2000} />
        <NumRow label="Kd (×1000)" value={d.kd_milli} onChange={f("kd_milli")} min={0} max={3000} />
        <NumRow label="α grid (×1000)" value={d.alpha_grid_milli} onChange={f("alpha_grid_milli")} min={10} max={1000} />
        <NumRow label="α setpoint (×1000)" value={d.alpha_setpoint_milli} onChange={f("alpha_setpoint_milli")} min={10} max={1000} />
        <NumRow label="Deadband" value={d.deadband_w} onChange={f("deadband_w")} min={0} max={1000} unit="W" />
        <NumRow label="Safety-zero after N fails" value={d.safety_zero_after_failures} onChange={f("safety_zero_after_failures")} min={0} max={100} />
        <NumRow label="Keep-alive interval" value={d.keepalive_interval_s} onChange={f("keepalive_interval_s")} min={0} max={1800} unit="s" />
        <NumRow label="Keep-alive pulse" value={d.keepalive_pulse_w} onChange={f("keepalive_pulse_w")} min={0} max={500} unit="W" />
      </div>
      <div class="commands">
        <button disabled={!dirty} onClick={() => put("/sbse_controller/active_config", d)}>Apply</button>
        <button disabled={!dirty} onClick={() => setD(active)}>Reset</button>
      </div>
    </section>
  );
}

function ConfigCard({ config }: { config: Config }) {
  const [d, setD] = useState<Config>(config);
  const dirty = JSON.stringify(d) !== JSON.stringify(config);
  useEffect(() => { if (!dirty) setD(config); }, [config]);
  const n = (k: keyof Config) => (v: number) => setD({ ...d, [k]: v });
  return (
    <section class="card">
      <h2>Persistent config <small>(reboot for init-only fields)</small></h2>
      <div class="grid2">
        <label class="row"><span class="row-label">Enabled</span>
          <input type="checkbox" checked={d.enabled} onInput={(e) => setD({ ...d, enabled: (e.target as HTMLInputElement).checked })} /></label>
        <label class="row"><span class="row-label">Inverter host</span>
          <input type="text" value={d.host} onInput={(e) => setD({ ...d, host: (e.target as HTMLInputElement).value })} /></label>
        <NumRow label="Inverter port" value={d.port} onChange={n("port")} min={1} max={65535} />
        <NumRow label="Tick" value={d.tick_ms} onChange={n("tick_ms")} min={50} max={5000} unit="ms" />
        <NumRow label="SoC interval" value={d.soc_interval_ms} onChange={n("soc_interval_ms")} min={100} max={60000} unit="ms" />
        <label class="row"><span class="row-label">Modbus server</span>
          <input type="checkbox" checked={d.modbus_server_enabled} onInput={(e) => setD({ ...d, modbus_server_enabled: (e.target as HTMLInputElement).checked })} /></label>
        <NumRow label="Server port" value={d.modbus_server_port} onChange={n("modbus_server_port")} min={1} max={65535} />
        <NumRow label="Server unit id" value={d.modbus_server_unit_id} onChange={n("modbus_server_unit_id")} min={0} max={247} />
        <NumRow label="Server watchdog" value={d.modbus_server_watchdog_s} onChange={n("modbus_server_watchdog_s")} min={0} max={3600} unit="s" />
        <NumRow label="Authority (0/1/2)" value={d.modbus_server_authority} onChange={n("modbus_server_authority")} min={0} max={2} />
      </div>
      <div class="commands">
        <button disabled={!dirty} onClick={() => put("/sbse_controller/config", d)}>Save</button>
        <button disabled={!dirty} onClick={() => setD(config)}>Reset</button>
      </div>
    </section>
  );
}

render(<App />, document.getElementById("app")!);
