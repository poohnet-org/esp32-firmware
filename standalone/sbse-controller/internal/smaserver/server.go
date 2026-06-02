// Package smaserver is the SMA-compatible Modbus TCP server + read-side proxy.
// It is a thin protocol adapter (firmware: sbse_modbus_server.{h,cpp}): it accepts
// the SMA register blocks an evcc / WARP "SMA Hybrid Inverter" client sends and
// forwards them to a Backend that carries the semantics. Reads (FC3/FC4) are served
// from the Proxy cache.
package smaserver

import (
	"fmt"
	"log"

	"github.com/simonvetter/modbus"
)

// SMA register blocks we accept on writes.
const (
	opModAddr     = 40236
	opModRegCount = 2
	setpAddrLo    = 40793
	setpRegMax    = 10 // 5 x uint32
)

// Backend carries the controller semantics the protocol adapter delegates to.
type Backend interface {
	OnOpMod(opMod uint32) error
	OnSetpoint(startAddr, regCount uint16, regs []uint16) error
	SynthInputs() (gridW, batteryW int32, socPct uint8)
	NoteRead()
	ServerUnitID() uint8 // 0 = accept any unit id
}

// Server wraps a simonvetter Modbus server bound to a configured port.
type Server struct {
	backend Backend
	proxy   *Proxy
	url     string
	srv     *modbus.ModbusServer
}

// New builds (but does not start) a server listening on the given port.
func New(port uint16, backend Backend, proxy *Proxy) *Server {
	return &Server{
		backend: backend,
		proxy:   proxy,
		url:     fmt.Sprintf("tcp://[::]:%d", port),
	}
}

// Start binds the listener and begins serving.
func (s *Server) Start() error {
	srv, err := modbus.NewServer(&modbus.ServerConfiguration{
		URL:        s.url,
		MaxClients: 8,
		Logger:     log.New(log.Writer(), "sbse-mb: ", 0),
	}, &handler{backend: s.backend, proxy: s.proxy})
	if err != nil {
		return err
	}
	if err := srv.Start(); err != nil {
		return err
	}
	s.srv = srv
	return nil
}

// Stop stops the listener.
func (s *Server) Stop() {
	if s.srv != nil {
		_ = s.srv.Stop()
		s.srv = nil
	}
}

// handler implements modbus.RequestHandler.
type handler struct {
	backend Backend
	proxy   *Proxy
}

func (h *handler) accept(unitID uint8) bool {
	want := h.backend.ServerUnitID()
	return want == 0 || unitID == want
}

// HandleInputRegisters serves FC4 reads from the proxy cache.
func (h *handler) HandleInputRegisters(req *modbus.InputRegistersRequest) ([]uint16, error) {
	if !h.accept(req.UnitId) {
		return nil, modbus.ErrIllegalDataAddress
	}
	grid, battery, soc := h.backend.SynthInputs()
	res := h.proxy.Pack(req.Addr, req.Quantity, grid, battery, soc)
	h.backend.NoteRead()
	return res, nil
}

// HandleHoldingRegisters serves FC3 reads (from the proxy) and FC16/FC6 writes
// (OpMod + setpoint sub-block) via the backend.
func (h *handler) HandleHoldingRegisters(req *modbus.HoldingRegistersRequest) ([]uint16, error) {
	if !h.accept(req.UnitId) {
		return nil, modbus.ErrIllegalDataAddress
	}

	if !req.IsWrite {
		grid, battery, soc := h.backend.SynthInputs()
		res := h.proxy.Pack(req.Addr, req.Quantity, grid, battery, soc)
		h.backend.NoteRead()
		return res, nil
	}

	// OpMod (40236, exactly 2 registers).
	if req.Addr == opModAddr && req.Quantity == opModRegCount {
		opMod := (uint32(req.Args[0]) << 16) | uint32(req.Args[1])
		if err := h.backend.OnOpMod(opMod); err != nil {
			return nil, modbus.ErrServerDeviceFailure
		}
		return nil, nil
	}

	// Setpoint sub-block: any even-aligned 2/4/6/8/10-register window in [40793,40802].
	if req.Addr >= setpAddrLo {
		off := req.Addr - setpAddrLo
		if off%2 == 0 && req.Quantity > 0 && req.Quantity%2 == 0 && off+req.Quantity <= setpRegMax {
			if err := h.backend.OnSetpoint(req.Addr, req.Quantity, req.Args); err != nil {
				return nil, modbus.ErrServerDeviceFailure
			}
			return nil, nil
		}
	}

	return nil, modbus.ErrIllegalDataAddress
}

// HandleCoils / HandleDiscreteInputs are unsupported.
func (h *handler) HandleCoils(*modbus.CoilsRequest) ([]bool, error) {
	return nil, modbus.ErrIllegalFunction
}

func (h *handler) HandleDiscreteInputs(*modbus.DiscreteInputsRequest) ([]bool, error) {
	return nil, modbus.ErrIllegalFunction
}
