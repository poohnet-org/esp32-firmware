package inverter

import (
	"fmt"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/simonvetter/modbus"
)

// fakeSBSE is a minimal Modbus server standing in for the SBSE inverter so the
// client's register decoding is exercised over a real Modbus exchange.
type fakeSBSE struct {
	mu           sync.Mutex
	lastSetpoint []uint16
}

func i32regs(v int32) []uint16 {
	u := uint32(v)
	return []uint16{uint16(u >> 16), uint16(u & 0xFFFF)}
}

func (f *fakeSBSE) HandleInputRegisters(req *modbus.InputRegistersRequest) ([]uint16, error) {
	switch req.Addr {
	case gridPowerAddr: // export-positive on the wire
		return i32regs(1000), nil
	case batteryPowerAddr: // charge=300 @0, discharge=0 @6 -> battery_w = -300
		regs := make([]uint16, 8)
		copy(regs[0:2], i32regs(300))
		copy(regs[6:8], i32regs(0))
		return regs, nil
	case batterySocAddr:
		return i32regs(75), nil
	}
	return make([]uint16, req.Quantity), nil
}

func (f *fakeSBSE) HandleHoldingRegisters(req *modbus.HoldingRegistersRequest) ([]uint16, error) {
	if req.IsWrite && req.Addr == powerSetpointAddr {
		f.mu.Lock()
		f.lastSetpoint = append([]uint16(nil), req.Args...)
		f.mu.Unlock()
		return nil, nil
	}
	if !req.IsWrite {
		return make([]uint16, req.Quantity), nil
	}
	return nil, modbus.ErrIllegalDataAddress
}

func (f *fakeSBSE) HandleCoils(*modbus.CoilsRequest) ([]bool, error) {
	return nil, modbus.ErrIllegalFunction
}
func (f *fakeSBSE) HandleDiscreteInputs(*modbus.DiscreteInputsRequest) ([]bool, error) {
	return nil, modbus.ErrIllegalFunction
}

func freePort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	_ = ln.Close()
	return port
}

func TestClientRegisterDecoding(t *testing.T) {
	port := freePort(t)
	fake := &fakeSBSE{}
	srv, err := modbus.NewServer(&modbus.ServerConfiguration{
		URL:        fmt.Sprintf("tcp://127.0.0.1:%d", port),
		MaxClients: 4,
	}, fake)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.Start(); err != nil {
		t.Fatal(err)
	}
	defer srv.Stop()

	c := New("127.0.0.1", uint16(port), 2*time.Second)
	if err := c.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer c.Close()

	grid, err := c.ReadGridPowerW()
	if err != nil {
		t.Fatalf("grid: %v", err)
	}
	if grid != -1000 { // wire 1000 export-positive -> negated to import-positive
		t.Errorf("grid = %d, want -1000", grid)
	}

	battery, err := c.ReadBatteryPowerW()
	if err != nil {
		t.Fatalf("battery: %v", err)
	}
	if battery != -300 { // discharge(0) - charge(300)
		t.Errorf("battery = %d, want -300", battery)
	}

	soc, err := c.ReadSocPct()
	if err != nil {
		t.Fatalf("soc: %v", err)
	}
	if soc != 75 {
		t.Errorf("soc = %d, want 75", soc)
	}

	if err := c.WriteSetpointW(1234); err != nil {
		t.Fatalf("write: %v", err)
	}
	fake.mu.Lock()
	got := fake.lastSetpoint
	fake.mu.Unlock()
	want := append(i32regs(1234), i32regs(sbseCompanionValue)...)
	if len(got) != len(want) {
		t.Fatalf("setpoint regs = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("setpoint reg[%d] = %#04x, want %#04x", i, got[i], want[i])
		}
	}
}
