package api

import "sync"

// event is one named SSE message (event name + JSON payload).
type event struct {
	name string
	data []byte
}

// Hub fans out named events to all connected SSE subscribers. The latest payload
// per event name is retained so a freshly connected client gets the current state
// immediately.
type Hub struct {
	mu          sync.Mutex
	subscribers map[chan event]struct{}
	last        map[string][]byte
}

// NewHub creates an empty hub.
func NewHub() *Hub {
	return &Hub{
		subscribers: make(map[chan event]struct{}),
		last:        make(map[string][]byte),
	}
}

// Broadcast publishes data under the given event name to all subscribers and
// retains it as the latest value for that name.
func (h *Hub) Broadcast(name string, data []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.last[name] = data
	for ch := range h.subscribers {
		// Non-blocking send: a slow client drops the in-between updates rather
		// than stalling the broadcaster.
		select {
		case ch <- event{name: name, data: data}:
		default:
		}
	}
}

// subscribe registers a new subscriber and returns its channel plus a snapshot of
// the latest retained events to replay immediately.
func (h *Hub) subscribe() (chan event, []event) {
	h.mu.Lock()
	defer h.mu.Unlock()
	ch := make(chan event, 16)
	h.subscribers[ch] = struct{}{}
	replay := make([]event, 0, len(h.last))
	for name, data := range h.last {
		replay = append(replay, event{name: name, data: data})
	}
	return ch, replay
}

func (h *Hub) unsubscribe(ch chan event) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, ok := h.subscribers[ch]; ok {
		delete(h.subscribers, ch)
		close(ch)
	}
}
