// Package inverter is the Modbus-TCP client to the SMA SBSE hybrid inverter.
//
// The register map is ported verbatim from the firmware
// (software/src/modules/sbse_controller/sbse_control_loop.cpp:47-107). SBSE-only
// by design: addresses, byte order and the -15000 companion value are hard-coded.
//
// The control loop and the proxy poller share this one connection, exactly as the
// firmware's TFModbusTCPClientPool serialises transactions on a single socket.
// Every public operation takes the mutex, so reads and writes never interleave on
// the wire.
package inverter

import (
	"fmt"
	"sync"
	"time"

	"github.com/simonvetter/modbus"
)

// SBSE register map (SMA "Modbus addresses", passed verbatim to the client).
const (
	GridMeterUnitID = 2
	InverterUnitID  = 3

	gridPowerAddr     = 31249 // int32be [W], positive = export -> negated to import-positive
	gridPowerRegCount = 2

	batteryPowerAddr       = 31585 // 8 regs: charge uint32be @0, discharge uint32be @6
	batteryPowerRegCount   = 8
	batteryChargeRegOff    = 0
	batteryDischargeRegOff = 6

	batterySocAddr     = 30845 // uint32be [%]
	batterySocRegCount = 2

	powerSetpointAddr     = 41467 // FC16: setpoint int32be + companion int32be
	powerSetpointRegCount = 4
	sbseCompanionValue    = -15000 // empirically required alongside the setpoint
)

// Client is a mutex-guarded SBSE Modbus client.
type Client struct {
	mu      sync.Mutex
	url     string
	timeout time.Duration
	mc      *modbus.ModbusClient
}

// New builds a client for host:port with the given per-transaction timeout.
// Call Connect before issuing operations.
func New(host string, port uint16, timeout time.Duration) *Client {
	return &Client{
		url:     fmt.Sprintf("tcp://%s:%d", host, port),
		timeout: timeout,
	}
}

// Connect (re)establishes the TCP connection. Safe to call again to reconnect;
// any previous connection is closed first.
func (c *Client) Connect() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mc != nil {
		_ = c.mc.Close()
		c.mc = nil
	}
	mc, err := modbus.NewClient(&modbus.ClientConfiguration{
		URL:     c.url,
		Timeout: c.timeout,
	})
	if err != nil {
		return err
	}
	if err := mc.Open(); err != nil {
		return err
	}
	c.mc = mc
	return nil
}

// Close drops the connection.
func (c *Client) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mc != nil {
		_ = c.mc.Close()
		c.mc = nil
	}
}

// Connected reports whether a connection is currently established.
func (c *Client) Connected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.mc != nil
}

// readInput reads count input registers (FC4) at addr from unit. Caller holds mu.
func (c *Client) readInputLocked(unit uint8, addr, count uint16) ([]uint16, error) {
	if c.mc == nil {
		return nil, fmt.Errorf("not connected")
	}
	if err := c.mc.SetUnitId(unit); err != nil {
		return nil, err
	}
	return c.mc.ReadRegisters(addr, count, modbus.INPUT_REGISTER)
}

// ReadInput reads count input registers at addr from unit. Used by the proxy poller.
func (c *Client) ReadInput(unit uint8, addr, count uint16) ([]uint16, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.readInputLocked(unit, addr, count)
}

// ReadGridPowerW returns grid power in W with import positive (firmware negates the
// wire value, which is export-positive).
func (c *Client) ReadGridPowerW() (int32, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	regs, err := c.readInputLocked(GridMeterUnitID, gridPowerAddr, gridPowerRegCount)
	if err != nil {
		return 0, err
	}
	return -readInt32be(regs), nil
}

// ReadBatteryPowerW returns battery power in W, positive = discharging,
// negative = charging (discharge - charge).
func (c *Client) ReadBatteryPowerW() (int32, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	regs, err := c.readInputLocked(InverterUnitID, batteryPowerAddr, batteryPowerRegCount)
	if err != nil {
		return 0, err
	}
	charge := readUint32be(regs[batteryChargeRegOff:])
	discharge := readUint32be(regs[batteryDischargeRegOff:])
	return int32(discharge) - int32(charge), nil
}

// ReadSocPct returns the battery state of charge in percent, clamped to 0..100.
func (c *Client) ReadSocPct() (uint8, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	regs, err := c.readInputLocked(InverterUnitID, batterySocAddr, batterySocRegCount)
	if err != nil {
		return 0, err
	}
	v := readUint32be(regs)
	if v > 100 {
		v = 100
	}
	return uint8(v), nil
}

// WriteSetpointW writes the battery active-power setpoint (FC16) plus the SBSE
// companion value.
func (c *Client) WriteSetpointW(watts int32) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mc == nil {
		return fmt.Errorf("not connected")
	}
	regs := make([]uint16, 0, powerSetpointRegCount)
	regs = appendInt32be(regs, watts)
	regs = appendInt32be(regs, sbseCompanionValue)
	if err := c.mc.SetUnitId(InverterUnitID); err != nil {
		return err
	}
	return c.mc.WriteRegisters(powerSetpointAddr, regs)
}

// readInt32be assembles a big-endian int32 from two host-order registers
// (regs[0] = high half), matching the firmware's read_int32be.
func readInt32be(regs []uint16) int32 {
	return int32((uint32(regs[0]) << 16) | uint32(regs[1]))
}

func readUint32be(regs []uint16) uint32 {
	return (uint32(regs[0]) << 16) | uint32(regs[1])
}

func appendInt32be(regs []uint16, v int32) []uint16 {
	u := uint32(v)
	return append(regs, uint16(u>>16), uint16(u&0xFFFF))
}
