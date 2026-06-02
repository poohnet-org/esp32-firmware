// Package trace is the 5-minute, 1 Hz live-trace ring buffer served at
// GET /sbse_controller/history (firmware: sbse_trace_history.{h,cpp}).
package trace

import (
	"strconv"
	"sync"
	"time"
)

const (
	// Capacity is 5 minutes at 1 Hz.
	Capacity = 300
	// SampleInterval is the internal throttle: extra AddSample calls are dropped.
	SampleInterval = time.Second
)

type sample struct {
	captured                        time.Time
	grid, battery, setpoint, lo, hi int16
}

// History is a fixed-capacity ring buffer of samples.
type History struct {
	mu      sync.Mutex
	now     func() time.Time
	samples [Capacity]sample
	count   int
	head    int
	last    time.Time
}

// New builds a History. clock may be nil (time.Now is used).
func New(clock func() time.Time) *History {
	if clock == nil {
		clock = time.Now
	}
	return &History{now: clock}
}

// AddSample captures one sample, throttled to one per SampleInterval.
func (h *History) AddSample(gridW, batteryW, setpointW, targetLoW, targetHiW int32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()
	if !h.last.IsZero() && now.Sub(h.last) < SampleInterval {
		return
	}
	h.last = now
	h.samples[h.head] = sample{
		captured: now,
		grid:     sat16(gridW),
		battery:  sat16(batteryW),
		setpoint: sat16(setpointW),
		lo:       sat16(targetLoW),
		hi:       sat16(targetHiW),
	}
	h.head = (h.head + 1) % Capacity
	if h.count < Capacity {
		h.count++
	}
}

// JSON renders {"samples":[[age_ms,grid,battery,setpoint,lo,hi], ...]} oldest first.
// age_ms is computed at render time so the browser can rebase to its own clock.
func (h *History) JSON() []byte {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()

	buf := make([]byte, 0, Capacity*48+16)
	buf = append(buf, `{"samples":[`...)
	if h.count > 0 {
		start := (h.head + Capacity - h.count) % Capacity
		for i := 0; i < h.count; i++ {
			s := h.samples[(start+i)%Capacity]
			if i > 0 {
				buf = append(buf, ',')
			}
			ageMs := now.Sub(s.captured).Milliseconds()
			buf = append(buf, '[')
			buf = strconv.AppendInt(buf, ageMs, 10)
			buf = append(buf, ',')
			buf = strconv.AppendInt(buf, int64(s.grid), 10)
			buf = append(buf, ',')
			buf = strconv.AppendInt(buf, int64(s.battery), 10)
			buf = append(buf, ',')
			buf = strconv.AppendInt(buf, int64(s.setpoint), 10)
			buf = append(buf, ',')
			buf = strconv.AppendInt(buf, int64(s.lo), 10)
			buf = append(buf, ',')
			buf = strconv.AppendInt(buf, int64(s.hi), 10)
			buf = append(buf, ']')
		}
	}
	buf = append(buf, `]}`...)
	return buf
}

func sat16(v int32) int16 {
	const maxI16, minI16 = 32767, -32768
	if v > maxI16 {
		return maxI16
	}
	if v < minI16 {
		return minI16
	}
	return int16(v)
}
