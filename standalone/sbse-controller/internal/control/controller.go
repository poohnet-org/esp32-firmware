// Package control is the SBSE control loop and state machine: it reads the
// inverter, runs the control law (law.go), writes the setpoint, and exposes the
// same state/config/command surface as the firmware module.
//
// Concurrency: one goroutine drives Tick(). The state mutex (mu) guards every
// field shared with the SMA server handlers, the HTTP/MQTT handlers and the proxy
// synthesis reads. The mutex is NEVER held across a blocking Modbus transaction --
// Tick snapshots under lock, does I/O unlocked, then re-locks to commit.
package control

import (
	"math"
	"sync"
	"time"

	"github.com/poohnet/sbse-controller/internal/config"
)

const pauseDuration = 30 * time.Second

// InverterIO is the subset of the inverter client the loop needs (lets tests
// substitute a fake).
type InverterIO interface {
	Connected() bool
	Connect() error
	Close()
	ReadGridPowerW() (int32, error)
	ReadBatteryPowerW() (int32, error)
	ReadSocPct() (uint8, error)
	WriteSetpointW(int32) error
}

// TraceSink receives one sample per tick (it throttles internally to 1 Hz).
type TraceSink interface {
	AddSample(gridW, batteryW, setpointW, targetLoW, targetHiW int32)
}

// Hooks are fired (outside the state mutex) when published data changes, so the
// API/MQTT layers can push updates.
type Hooks struct {
	OnState        func(State)
	OnActiveConfig func(config.ActiveConfig)
	OnConfig       func(config.Config)
}

// State is the read-only published state (firmware: sbse_controller/state).
type State struct {
	Mode             string `json:"mode"`
	LastSetpointW    int32  `json:"last_setpoint_w"`
	LastWriteAgeMs   uint32 `json:"last_write_age_ms"`
	GridWRaw         int32  `json:"grid_w_raw"`
	GridWEma         int32  `json:"grid_w_ema"`
	BatteryW         int32  `json:"battery_w"`
	BatterySoc       uint8  `json:"battery_soc"`
	WriteOkCount     uint32 `json:"write_ok_count"`
	WriteErrCount    uint32 `json:"write_err_count"`
	ReadFailStreak   uint32 `json:"read_fail_streak"`
	ModbusActive     bool   `json:"modbus_active"`
	ModbusOpMod      uint16 `json:"modbus_op_mod"`
	ModbusForceW     int32  `json:"modbus_force_w"`
	ModbusReadCount  uint32 `json:"modbus_read_count"`
	ModbusWriteCount uint32 `json:"modbus_write_count"`
	LastError        string `json:"last_error"`
}

// Controller owns all runtime state.
type Controller struct {
	mu sync.Mutex

	inv   InverterIO
	trace TraceSink
	hooks Hooks
	now   func() time.Time
	logf  func(string, ...any)

	// Persistent config + live overlay.
	cfg    config.Config
	active config.ActiveConfig

	// Cached runtime fields (refreshed by applyRuntimeFromActive).
	kp, kd                    float64
	alphaGrid, alphaSetpoint  float64
	lo, hi                    int32
	maxChargeW, maxDischargeW int32
	deadbandW                 int32
	safetyZeroAfterFails      uint32
	keepaliveIntervalS        uint32
	keepalivePulseW           int32

	// Init-only (from cfg).
	enabled       bool
	tickMs        uint32
	socIntervalMs uint32

	// SMA server runtime knobs (consulted live).
	modbusServerUnitID    uint8
	modbusServerWatchdog  time.Duration
	modbusServerAuthority Authority

	// Modbus force-mode state.
	modbusOpMod     uint16
	modbusForceW    int32
	modbusActive    bool
	lastModbusWrite time.Time

	// Control-law state.
	law LawState

	// Runtime.
	paused          bool
	pausedUntil     time.Time
	lastSocRead     time.Time
	lastWriteOK     time.Time
	gridWRaw        int32
	batteryWRaw     int32
	socPct          uint8
	lastWrittenW    int32
	consecutiveFail uint32
	safetyZeroArmed bool

	// Keep-alive bookkeeping.
	batteryIdleSince     time.Time
	keepaliveNextCharge  bool
	keepalivePendingZero bool

	// Published state mirror.
	mode             Mode
	writeOkCount     uint32
	writeErrCount    uint32
	modbusReadCount  uint32
	modbusWriteCount uint32
	lastError        string
}

// New builds a Controller seeded from cfg. clock and logf may be nil (defaults used).
func New(cfg config.Config, inv InverterIO, trace TraceSink, hooks Hooks,
	clock func() time.Time, logf func(string, ...any)) *Controller {
	if clock == nil {
		clock = time.Now
	}
	if logf == nil {
		logf = func(string, ...any) {}
	}
	c := &Controller{
		inv:         inv,
		trace:       trace,
		hooks:       hooks,
		now:         clock,
		logf:        logf,
		cfg:         cfg,
		active:      cfg.ToActive(),
		mode:        ModeDisabled,
		socPct:      255,
		modbusOpMod: OpModDefault,
	}
	c.loadInitOnly()
	c.applyRuntimeFromActive()
	return c
}

func (c *Controller) loadInitOnly() {
	c.enabled = c.cfg.Enabled
	c.tickMs = c.cfg.TickMs
	c.socIntervalMs = c.cfg.SocIntervalMs
	c.modbusServerUnitID = c.cfg.ModbusServerUnitID
	c.modbusServerWatchdog = time.Duration(c.cfg.ModbusServerWatchdogS) * time.Second
	c.modbusServerAuthority = Authority(c.cfg.ModbusServerAuthority)
}

// applyRuntimeFromActive refreshes the cached hot-path fields from active_config
// (firmware apply_runtime_from_active). Caller holds mu.
func (c *Controller) applyRuntimeFromActive() {
	c.lo = c.active.GridChargeTargetW
	c.hi = c.active.GridDischargeTargetW
	c.maxChargeW = int32(c.active.MaxChargeW)
	c.maxDischargeW = int32(c.active.MaxDischargeW)
	c.kp = float64(c.active.KpMilli) / 1000.0
	c.kd = float64(c.active.KdMilli) / 1000.0
	c.alphaGrid = float64(c.active.AlphaGridMilli) / 1000.0
	c.alphaSetpoint = float64(c.active.AlphaSetpointMilli) / 1000.0
	c.deadbandW = int32(c.active.DeadbandW)
	c.safetyZeroAfterFails = c.active.SafetyZeroAfterFails
	c.keepaliveIntervalS = c.active.KeepaliveIntervalS
	c.keepalivePulseW = int32(c.active.KeepalivePulseW)

	// Refresh the mode pill if we're in a running-family state (a cap change can
	// flip running <-> block_* <-> force_*).
	switch c.mode {
	case ModeRunning, ModeBlocked, ModeBlockCharge, ModeBlockDischarge,
		ModeForceCharge, ModeForceDischarge:
		c.mode = runningMode(c.modbusForceW, c.maxChargeW, c.maxDischargeW)
	}
}

// --- accessors used by main to build collaborators -------------------------

func (c *Controller) Enabled() bool               { return c.cfg.Enabled }
func (c *Controller) Host() string                { return c.cfg.Host }
func (c *Controller) Port() uint16                { return c.cfg.Port }
func (c *Controller) TickInterval() time.Duration { return time.Duration(c.tickMs) * time.Millisecond }
func (c *Controller) ModbusServerEnabled() bool   { return c.cfg.ModbusServerEnabled }
func (c *Controller) ModbusServerPort() uint16    { return c.cfg.ModbusServerPort }

// ServerUnitID is the live-tunable unit id the SMA server answers to (0 = any).
func (c *Controller) ServerUnitID() uint8 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.modbusServerUnitID
}

func (c *Controller) socInterval() time.Duration {
	return time.Duration(c.socIntervalMs) * time.Millisecond
}

// --- snapshots --------------------------------------------------------------

// Snapshot returns the current published state.
func (c *Controller) Snapshot() State {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.snapshotLocked()
}

func (c *Controller) snapshotLocked() State {
	var ageMs uint32
	if !c.lastWriteOK.IsZero() {
		ageMs = uint32(c.now().Sub(c.lastWriteOK).Milliseconds())
	}
	return State{
		Mode:             c.mode.String(),
		LastSetpointW:    c.lastWrittenW,
		LastWriteAgeMs:   ageMs,
		GridWRaw:         c.gridWRaw,
		GridWEma:         int32(math.Round(c.law.EmaGrid)),
		BatteryW:         c.batteryWRaw,
		BatterySoc:       c.socPct,
		WriteOkCount:     c.writeOkCount,
		WriteErrCount:    c.writeErrCount,
		ReadFailStreak:   c.consecutiveFail,
		ModbusActive:     c.modbusActive,
		ModbusOpMod:      c.modbusOpMod,
		ModbusForceW:     c.modbusForceW,
		ModbusReadCount:  c.modbusReadCount,
		ModbusWriteCount: c.modbusWriteCount,
		LastError:        c.lastError,
	}
}

// ConfigSnapshot returns a copy of the persistent config.
func (c *Controller) ConfigSnapshot() config.Config {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.cfg
}

// ActiveSnapshot returns a copy of the active config.
func (c *Controller) ActiveSnapshot() config.ActiveConfig {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.active
}

// notify fires the state hook outside the mutex.
func (c *Controller) notifyState() {
	if c.hooks.OnState != nil {
		c.hooks.OnState(c.Snapshot())
	}
}

func (c *Controller) notifyActive(ac config.ActiveConfig) {
	if c.hooks.OnActiveConfig != nil {
		c.hooks.OnActiveConfig(ac)
	}
}

func (c *Controller) setModeLocked(m Mode) {
	c.mode = m
}

// --- commands (HTTP / MQTT / dashboard) -------------------------------------

// UpdateConfig validates and applies a new persistent config, hot-reloading the
// live-tunable subset. Init-only fields take effect on the next restart. The
// caller is responsible for persisting (via Hooks.OnConfig, fired here).
func (c *Controller) UpdateConfig(newCfg config.Config) error {
	if err := newCfg.Validate(); err != nil {
		return err
	}
	c.mu.Lock()
	c.cfg = newCfg
	// Hot-reload live-tunable subset.
	c.active = newCfg.ToActive()
	c.applyRuntimeFromActive()
	// Live-tunable server knobs.
	c.modbusServerUnitID = newCfg.ModbusServerUnitID
	c.modbusServerWatchdog = time.Duration(newCfg.ModbusServerWatchdogS) * time.Second
	c.modbusServerAuthority = Authority(newCfg.ModbusServerAuthority)
	ac := c.active
	c.mu.Unlock()

	if c.hooks.OnConfig != nil {
		c.hooks.OnConfig(newCfg)
	}
	c.notifyActive(ac)
	c.notifyState()
	return nil
}

// UpdateActiveConfig validates and applies a new active config. This is the
// operator-takeover path: it clears any Modbus force-mode.
func (c *Controller) UpdateActiveConfig(ac config.ActiveConfig) error {
	if err := ac.Validate(); err != nil {
		return err
	}
	c.mu.Lock()
	c.active = ac
	c.applyRuntimeFromActive()
	if c.modbusForceW != 0 || c.modbusActive {
		c.logf("operator override -- releasing Modbus force-mode setpoint")
	}
	c.clearModbusLocked()
	c.mu.Unlock()

	c.notifyActive(ac)
	c.notifyState()
	return nil
}

// Pause writes one 0 W setpoint and holds the loop idle for 30 s.
func (c *Controller) Pause() {
	c.mu.Lock()
	c.paused = true
	c.pausedUntil = c.now().Add(pauseDuration)
	c.clearModbusLocked()
	c.batteryIdleSince = time.Time{}
	c.keepalivePendingZero = false
	c.setModeLocked(ModePaused)
	connected := c.inv.Connected()
	c.mu.Unlock()

	if connected {
		_ = c.inv.WriteSetpointW(0) // best effort
	}
	c.logf("pause: holding setpoint loop at 0 W for %.0f s", pauseDuration.Seconds())
	c.notifyState()
}

// Resume ends a pause early (no-op if not paused). Also clears Modbus force-mode.
func (c *Controller) Resume() {
	c.mu.Lock()
	if !c.paused {
		c.mu.Unlock()
		return
	}
	c.paused = false
	c.pausedUntil = time.Time{}
	c.law.Reset()
	c.batteryIdleSince = time.Time{}
	c.keepalivePendingZero = false
	c.clearModbusLocked()
	switch {
	case !c.enabled:
		c.setModeLocked(ModeDisabled)
	case !c.inv.Connected():
		c.setModeLocked(ModeNotConnected)
	default:
		c.setModeLocked(ModeStale)
	}
	c.mu.Unlock()
	c.logf("resume: ending pause early")
	c.notifyState()
}

// clearModbusLocked drops force-mode state (operator takeover / watchdog).
func (c *Controller) clearModbusLocked() {
	c.modbusForceW = 0
	c.modbusOpMod = OpModDefault
	c.modbusActive = false
}

// --- watchdog ---------------------------------------------------------------

// watchdogTick reverts overrides if no Modbus write arrived within the timeout.
// Caller holds mu.
func (c *Controller) watchdogTick() {
	if !c.modbusActive || c.modbusServerWatchdog == 0 || c.lastModbusWrite.IsZero() {
		return
	}
	if c.now().Sub(c.lastModbusWrite) < c.modbusServerWatchdog {
		return
	}
	c.logf("modbus watchdog: no client traffic for %ds, reverting to persistent config",
		int(c.modbusServerWatchdog.Seconds()))
	c.revertModbusOverridesLocked()
}

// revertModbusOverridesLocked restores the live-tunable subset from cfg and clears
// Modbus state (firmware revert_modbus_overrides). Caller holds mu.
func (c *Controller) revertModbusOverridesLocked() {
	c.active = c.cfg.ToActive()
	c.applyRuntimeFromActive()
	c.clearModbusLocked()
	c.lastModbusWrite = time.Time{}
}
