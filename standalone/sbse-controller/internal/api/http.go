// Package api exposes the controller over HTTP (REST + SSE) and serves the
// embedded dashboard. Endpoint paths and payloads mirror the firmware module's
// (see software/src/modules/sbse_controller/CONFIG.md).
package api

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"

	"github.com/poohnet/sbse-controller/internal/control"
	"github.com/poohnet/sbse-controller/internal/trace"
)

// Server wires the controller, trace history, SSE hub and static dashboard onto
// an http.Handler.
type Server struct {
	ctrl   *control.Controller
	trace  *trace.History
	hub    *Hub
	static fs.FS
}

// NewServer builds the HTTP server.
func NewServer(ctrl *control.Controller, tr *trace.History, hub *Hub, static fs.FS) *Server {
	return &Server{ctrl: ctrl, trace: tr, hub: hub, static: static}
}

// Handler returns the configured router.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /sbse_controller/state", s.getState)
	mux.HandleFunc("GET /sbse_controller/state/sse", s.streamSSE)
	mux.HandleFunc("GET /sbse_controller/config", s.getConfig)
	mux.HandleFunc("PUT /sbse_controller/config", s.putConfig)
	mux.HandleFunc("GET /sbse_controller/active_config", s.getActiveConfig)
	mux.HandleFunc("PUT /sbse_controller/active_config", s.putActiveConfig)
	mux.HandleFunc("POST /sbse_controller/pause", s.postPause)
	mux.HandleFunc("POST /sbse_controller/resume", s.postResume)
	mux.HandleFunc("GET /sbse_controller/history", s.getHistory)
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})

	// Dashboard (everything else).
	mux.Handle("/", http.FileServer(http.FS(s.static)))
	return mux
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func (s *Server) getState(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.ctrl.Snapshot())
}

func (s *Server) getConfig(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.ctrl.ConfigSnapshot())
}

// putConfig merges the request body onto the current config (send only the fields
// you want to change) then validates + applies it.
func (s *Server) putConfig(w http.ResponseWriter, r *http.Request) {
	cur := s.ctrl.ConfigSnapshot()
	if err := json.NewDecoder(r.Body).Decode(&cur); err != nil {
		writeErr(w, http.StatusBadRequest, fmt.Sprintf("parse body: %v", err))
		return
	}
	if err := s.ctrl.UpdateConfig(cur); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, s.ctrl.ConfigSnapshot())
}

func (s *Server) getActiveConfig(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.ctrl.ActiveSnapshot())
}

func (s *Server) putActiveConfig(w http.ResponseWriter, r *http.Request) {
	cur := s.ctrl.ActiveSnapshot()
	if err := json.NewDecoder(r.Body).Decode(&cur); err != nil {
		writeErr(w, http.StatusBadRequest, fmt.Sprintf("parse body: %v", err))
		return
	}
	if err := s.ctrl.UpdateActiveConfig(cur); err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, s.ctrl.ActiveSnapshot())
}

func (s *Server) postPause(w http.ResponseWriter, _ *http.Request) {
	s.ctrl.Pause()
	w.WriteHeader(http.StatusOK)
}

func (s *Server) postResume(w http.ResponseWriter, _ *http.Request) {
	s.ctrl.Resume()
	w.WriteHeader(http.StatusOK)
}

func (s *Server) getHistory(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(s.trace.JSON())
}

// streamSSE pushes named events (state / active_config / config) as they change.
func (s *Server) streamSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeErr(w, http.StatusInternalServerError, "streaming unsupported")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	ch, replay := s.hub.subscribe()
	defer s.hub.unsubscribe(ch)

	write := func(e event) bool {
		if _, err := fmt.Fprintf(w, "event: %s\ndata: %s\n\n", e.name, e.data); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}

	for _, e := range replay {
		if !write(e) {
			return
		}
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case e, ok := <-ch:
			if !ok || !write(e) {
				return
			}
		}
	}
}
