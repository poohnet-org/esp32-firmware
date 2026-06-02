// Package config defines the persistent Config and runtime ActiveConfig for the
// standalone SBSE controller, plus deployment-level App settings.
//
// Config mirrors the firmware module's `sbse_controller/config` schema field for
// field (see software/src/modules/sbse_controller/sbse_controller.cpp pre_setup()).
// ActiveConfig is the live-tunable subset, held in RAM only and reseeded from
// Config on boot -- identical semantics to the firmware's active_config.
package config

import (
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// Config is the persistent boot configuration (firmware: NVS-backed `config`).
// JSON tags match the firmware field names exactly so the REST/MQTT payloads are
// interchangeable with the module's.
type Config struct {
	// Init-only (require a restart to take effect).
	Enabled       bool   `json:"enabled"`
	Host          string `json:"host"`
	Port          uint16 `json:"port"`
	TickMs        uint32 `json:"tick_ms"`
	SocIntervalMs uint32 `json:"soc_interval_ms"`

	// Live-tunable (mirrored into ActiveConfig).
	GridChargeTargetW    int32  `json:"grid_charge_target_w"`
	GridDischargeTargetW int32  `json:"grid_discharge_target_w"`
	MaxChargeW           uint32 `json:"max_charge_w"`
	MaxDischargeW        uint32 `json:"max_discharge_w"`
	KpMilli              uint32 `json:"kp_milli"`
	KdMilli              uint32 `json:"kd_milli"`
	AlphaGridMilli       uint32 `json:"alpha_grid_milli"`
	AlphaSetpointMilli   uint32 `json:"alpha_setpoint_milli"`
	DeadbandW            uint32 `json:"deadband_w"`
	SafetyZeroAfterFails uint32 `json:"safety_zero_after_failures"`
	KeepaliveIntervalS   uint32 `json:"keepalive_interval_s"`
	KeepalivePulseW      uint32 `json:"keepalive_pulse_w"`

	// SMA-compatible Modbus server (init-only except watchdog/unit/authority).
	ModbusServerEnabled   bool   `json:"modbus_server_enabled"`
	ModbusServerPort      uint16 `json:"modbus_server_port"`
	ModbusServerUnitID    uint8  `json:"modbus_server_unit_id"`
	ModbusServerWatchdogS uint32 `json:"modbus_server_watchdog_s"`
	ModbusServerAuthority uint8  `json:"modbus_server_authority"` // 0=ForceOnly, 1=Caps, 2=Full
}

// ActiveConfig is the live-tunable runtime overlay (firmware: active_config).
type ActiveConfig struct {
	GridChargeTargetW    int32  `json:"grid_charge_target_w"`
	GridDischargeTargetW int32  `json:"grid_discharge_target_w"`
	MaxChargeW           uint32 `json:"max_charge_w"`
	MaxDischargeW        uint32 `json:"max_discharge_w"`
	KpMilli              uint32 `json:"kp_milli"`
	KdMilli              uint32 `json:"kd_milli"`
	AlphaGridMilli       uint32 `json:"alpha_grid_milli"`
	AlphaSetpointMilli   uint32 `json:"alpha_setpoint_milli"`
	DeadbandW            uint32 `json:"deadband_w"`
	SafetyZeroAfterFails uint32 `json:"safety_zero_after_failures"`
	KeepaliveIntervalS   uint32 `json:"keepalive_interval_s"`
	KeepalivePulseW      uint32 `json:"keepalive_pulse_w"`
}

// Default returns the firmware default Config.
func Default() Config {
	return Config{
		Enabled:               false,
		Host:                  "",
		Port:                  502,
		TickMs:                300,
		SocIntervalMs:         1000,
		GridChargeTargetW:     0,
		GridDischargeTargetW:  0,
		MaxChargeW:            5000,
		MaxDischargeW:         5000,
		KpMilli:               1000,
		KdMilli:               0,
		AlphaGridMilli:        300,
		AlphaSetpointMilli:    700,
		DeadbandW:             50,
		SafetyZeroAfterFails:  5,
		KeepaliveIntervalS:    480,
		KeepalivePulseW:       50,
		ModbusServerEnabled:   false,
		ModbusServerPort:      502,
		ModbusServerUnitID:    3,
		ModbusServerWatchdogS: 60,
		ModbusServerAuthority: 1,
	}
}

// ToActive returns the live-tunable subset of c (firmware: copy_live_tunable_to_active).
func (c Config) ToActive() ActiveConfig {
	return ActiveConfig{
		GridChargeTargetW:    c.GridChargeTargetW,
		GridDischargeTargetW: c.GridDischargeTargetW,
		MaxChargeW:           c.MaxChargeW,
		MaxDischargeW:        c.MaxDischargeW,
		KpMilli:              c.KpMilli,
		KdMilli:              c.KdMilli,
		AlphaGridMilli:       c.AlphaGridMilli,
		AlphaSetpointMilli:   c.AlphaSetpointMilli,
		DeadbandW:            c.DeadbandW,
		SafetyZeroAfterFails: c.SafetyZeroAfterFails,
		KeepaliveIntervalS:   c.KeepaliveIntervalS,
		KeepalivePulseW:      c.KeepalivePulseW,
	}
}

type rangeErr struct {
	field  string
	lo, hi int64
	got    int64
}

func (e rangeErr) Error() string {
	return fmt.Sprintf("%s out of range [%d..%d]: %d", e.field, e.lo, e.hi, e.got)
}

func chk(field string, got, lo, hi int64) error {
	if got < lo || got > hi {
		return rangeErr{field, lo, hi, got}
	}
	return nil
}

// Validate enforces the firmware field ranges and cross-field validators
// (sbse_controller.cpp:114-130). Returns the first violation found.
func (c Config) Validate() error {
	checks := []struct {
		f       string
		v, l, h int64
	}{
		{"port", int64(c.Port), 1, 65535},
		{"tick_ms", int64(c.TickMs), 50, 5000},
		{"soc_interval_ms", int64(c.SocIntervalMs), 100, 60000},
		{"grid_charge_target_w", int64(c.GridChargeTargetW), -750, 2500},
		{"grid_discharge_target_w", int64(c.GridDischargeTargetW), -750, 2500},
		{"max_charge_w", int64(c.MaxChargeW), 0, 10000},
		{"max_discharge_w", int64(c.MaxDischargeW), 0, 10000},
		{"kp_milli", int64(c.KpMilli), 100, 2000},
		{"kd_milli", int64(c.KdMilli), 0, 3000},
		{"alpha_grid_milli", int64(c.AlphaGridMilli), 10, 1000},
		{"alpha_setpoint_milli", int64(c.AlphaSetpointMilli), 10, 1000},
		{"deadband_w", int64(c.DeadbandW), 0, 1000},
		{"safety_zero_after_failures", int64(c.SafetyZeroAfterFails), 0, 100},
		{"keepalive_interval_s", int64(c.KeepaliveIntervalS), 0, 1800},
		{"keepalive_pulse_w", int64(c.KeepalivePulseW), 0, 500},
		{"modbus_server_port", int64(c.ModbusServerPort), 1, 65535},
		{"modbus_server_unit_id", int64(c.ModbusServerUnitID), 0, 247},
		{"modbus_server_watchdog_s", int64(c.ModbusServerWatchdogS), 0, 3600},
		{"modbus_server_authority", int64(c.ModbusServerAuthority), 0, 2},
	}
	for _, k := range checks {
		if err := chk(k.f, k.v, k.l, k.h); err != nil {
			return err
		}
	}
	// Cross-field validators.
	if c.GridDischargeTargetW < c.GridChargeTargetW {
		return fmt.Errorf("grid_discharge_target_w must be >= grid_charge_target_w")
	}
	if c.SocIntervalMs < c.TickMs {
		return fmt.Errorf("soc_interval_ms must be >= tick_ms")
	}
	if c.Enabled && strings.TrimSpace(c.Host) == "" {
		return fmt.Errorf("host must be set when enabled")
	}
	return nil
}

// Validate enforces the ActiveConfig ranges and the one cross-field rule.
func (a ActiveConfig) Validate() error {
	checks := []struct {
		f       string
		v, l, h int64
	}{
		{"grid_charge_target_w", int64(a.GridChargeTargetW), -750, 2500},
		{"grid_discharge_target_w", int64(a.GridDischargeTargetW), -750, 2500},
		{"max_charge_w", int64(a.MaxChargeW), 0, 10000},
		{"max_discharge_w", int64(a.MaxDischargeW), 0, 10000},
		{"kp_milli", int64(a.KpMilli), 100, 2000},
		{"kd_milli", int64(a.KdMilli), 0, 3000},
		{"alpha_grid_milli", int64(a.AlphaGridMilli), 10, 1000},
		{"alpha_setpoint_milli", int64(a.AlphaSetpointMilli), 10, 1000},
		{"deadband_w", int64(a.DeadbandW), 0, 1000},
		{"safety_zero_after_failures", int64(a.SafetyZeroAfterFails), 0, 100},
		{"keepalive_interval_s", int64(a.KeepaliveIntervalS), 0, 1800},
		{"keepalive_pulse_w", int64(a.KeepalivePulseW), 0, 500},
	}
	for _, k := range checks {
		if err := chk(k.f, k.v, k.l, k.h); err != nil {
			return err
		}
	}
	if a.GridDischargeTargetW < a.GridChargeTargetW {
		return fmt.Errorf("grid_discharge_target_w must be >= grid_charge_target_w")
	}
	return nil
}

// Load reads Config from path (JSON). A missing file yields Default() with no error
// so a fresh deployment boots cleanly. Env overrides are applied on top.
func Load(path string) (Config, error) {
	c := Default()
	if path != "" {
		data, err := os.ReadFile(path)
		switch {
		case err == nil:
			if err := json.Unmarshal(data, &c); err != nil {
				return c, fmt.Errorf("parse %s: %w", path, err)
			}
		case os.IsNotExist(err):
			// fall through to defaults
		default:
			return c, fmt.Errorf("read %s: %w", path, err)
		}
	}
	c.applyEnv()
	return c, nil
}

// Save writes Config to path as pretty JSON, creating parent dirs as needed.
func Save(path string, c Config) error {
	if path == "" {
		return nil
	}
	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// applyEnv overlays SBSE_* environment variables on the config. Useful for
// k8s ConfigMap-as-env. Unset or unparseable vars are ignored (defaults stand).
func (c *Config) applyEnv() {
	if v, ok := os.LookupEnv("SBSE_ENABLED"); ok {
		c.Enabled = truthy(v)
	}
	if v, ok := os.LookupEnv("SBSE_HOST"); ok {
		c.Host = v
	}
	envU16("SBSE_PORT", &c.Port)
	envU32("SBSE_TICK_MS", &c.TickMs)
	envU32("SBSE_SOC_INTERVAL_MS", &c.SocIntervalMs)
	envI32("SBSE_GRID_CHARGE_TARGET_W", &c.GridChargeTargetW)
	envI32("SBSE_GRID_DISCHARGE_TARGET_W", &c.GridDischargeTargetW)
	envU32("SBSE_MAX_CHARGE_W", &c.MaxChargeW)
	envU32("SBSE_MAX_DISCHARGE_W", &c.MaxDischargeW)
	envU32("SBSE_KP_MILLI", &c.KpMilli)
	envU32("SBSE_KD_MILLI", &c.KdMilli)
	envU32("SBSE_ALPHA_GRID_MILLI", &c.AlphaGridMilli)
	envU32("SBSE_ALPHA_SETPOINT_MILLI", &c.AlphaSetpointMilli)
	envU32("SBSE_DEADBAND_W", &c.DeadbandW)
	envU32("SBSE_SAFETY_ZERO_AFTER_FAILURES", &c.SafetyZeroAfterFails)
	envU32("SBSE_KEEPALIVE_INTERVAL_S", &c.KeepaliveIntervalS)
	envU32("SBSE_KEEPALIVE_PULSE_W", &c.KeepalivePulseW)
	if v, ok := os.LookupEnv("SBSE_MODBUS_SERVER_ENABLED"); ok {
		c.ModbusServerEnabled = truthy(v)
	}
	envU16("SBSE_MODBUS_SERVER_PORT", &c.ModbusServerPort)
	envU8("SBSE_MODBUS_SERVER_UNIT_ID", &c.ModbusServerUnitID)
	envU32("SBSE_MODBUS_SERVER_WATCHDOG_S", &c.ModbusServerWatchdogS)
	envU8("SBSE_MODBUS_SERVER_AUTHORITY", &c.ModbusServerAuthority)
}

func truthy(v string) bool {
	switch strings.ToLower(strings.TrimSpace(v)) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

func envU32(key string, dst *uint32) {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.ParseUint(strings.TrimSpace(v), 10, 32); err == nil {
			*dst = uint32(n)
		}
	}
}

func envU16(key string, dst *uint16) {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.ParseUint(strings.TrimSpace(v), 10, 16); err == nil {
			*dst = uint16(n)
		}
	}
}

func envU8(key string, dst *uint8) {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.ParseUint(strings.TrimSpace(v), 10, 8); err == nil {
			*dst = uint8(n)
		}
	}
}

func envI32(key string, dst *int32) {
	if v, ok := os.LookupEnv(key); ok {
		if n, err := strconv.ParseInt(strings.TrimSpace(v), 10, 32); err == nil {
			*dst = int32(n)
		}
	}
}
