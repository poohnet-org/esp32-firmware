// Command sbse-controller is the standalone SBSE battery controller: a Go port of
// the esp32-firmware sbse_controller module, packaged for Docker / k8s.
//
// It connects to an SMA SBSE hybrid inverter over Modbus TCP, runs the regime-based
// control law, exposes the same REST / MQTT / dashboard surface as the firmware, and
// hosts the SMA-compatible Modbus server + proxy for evcc / WARP clients.
package main

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"os/signal"
	"syscall"
	"time"

	"github.com/poohnet/sbse-controller/internal/api"
	"github.com/poohnet/sbse-controller/internal/config"
	"github.com/poohnet/sbse-controller/internal/control"
	"github.com/poohnet/sbse-controller/internal/inverter"
	"github.com/poohnet/sbse-controller/internal/smaserver"
	"github.com/poohnet/sbse-controller/internal/trace"
	webui "github.com/poohnet/sbse-controller/web"
)

const (
	modbusTimeout = 2 * time.Second
	proxyPoll     = 500 * time.Millisecond
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("sbse: ")

	app := config.AppFromEnv()
	cfg, err := config.Load(app.ConfigPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if err := cfg.Validate(); err != nil {
		log.Fatalf("invalid config: %v", err)
	}

	tr := trace.New(time.Now)
	proxy := smaserver.NewProxy()
	hub := api.NewHub()

	inv := inverter.New(cfg.Host, cfg.Port, modbusTimeout)

	// mqttClient is assigned after the controller is built (the MQTT subscriber
	// handlers need the controller). Hooks reference it lazily, guarding nil.
	var mqttClient *api.MQTT

	hooks := control.Hooks{
		OnState: func(s control.State) {
			if b, err := json.Marshal(s); err == nil {
				hub.Broadcast("state", b)
			}
			if mqttClient != nil {
				mqttClient.PublishState(s)
			}
		},
		OnActiveConfig: func(a config.ActiveConfig) {
			if b, err := json.Marshal(a); err == nil {
				hub.Broadcast("active_config", b)
			}
			if mqttClient != nil {
				mqttClient.PublishActive(a)
			}
		},
		OnConfig: func(c config.Config) {
			if err := config.Save(app.ConfigPath, c); err != nil {
				log.Printf("save config: %v", err)
			}
			if b, err := json.Marshal(c); err == nil {
				hub.Broadcast("config", b)
			}
			if mqttClient != nil {
				mqttClient.PublishConfig(c)
			}
		},
	}

	ctrl := control.New(cfg, inv, tr, hooks, time.Now, log.Printf)

	// Optional MQTT mirror.
	if app.MQTT.Enabled {
		if app.MQTT.Broker == "" {
			log.Println("mqtt enabled but SBSE_MQTT_BROKER is empty -- skipping")
		} else if mc, err := api.NewMQTT(app.MQTT, ctrl, log.Printf); err != nil {
			log.Printf("mqtt connect failed: %v (continuing without mqtt)", err)
		} else {
			mqttClient = mc
		}
	}

	// SMA-compatible Modbus server.
	var sma *smaserver.Server
	if cfg.ModbusServerEnabled {
		sma = smaserver.New(cfg.ModbusServerPort, ctrl, proxy)
		if err := sma.Start(); err != nil {
			log.Printf("sma modbus server failed to start on :%d: %v", cfg.ModbusServerPort, err)
			sma = nil
		} else {
			log.Printf("sma modbus server listening on :%d (unit %d)", cfg.ModbusServerPort, cfg.ModbusServerUnitID)
		}
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// HTTP server.
	httpSrv := &http.Server{
		Addr:    app.HTTPAddr,
		Handler: api.NewServer(ctrl, tr, hub, webui.FS()).Handler(),
	}
	go func() {
		log.Printf("http listening on %s", app.HTTPAddr)
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("http server: %v", err)
		}
	}()

	// Control loop.
	go runControlLoop(ctx, ctrl)

	// Proxy poller (round-robin upstream cache refresh).
	go runProxyPoller(ctx, inv, proxy)

	<-ctx.Done()
	log.Println("shutdown: writing 0 W and stopping")

	if sma != nil {
		sma.Stop()
	}
	if mqttClient != nil {
		mqttClient.Close()
	}
	ctrl.Shutdown()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(shutdownCtx)
}

func runControlLoop(ctx context.Context, ctrl *control.Controller) {
	ticker := time.NewTicker(ctrl.TickInterval())
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			ctrl.Tick()
		}
	}
}

func runProxyPoller(ctx context.Context, inv *inverter.Client, proxy *smaserver.Proxy) {
	ticker := time.NewTicker(proxyPoll)
	defer ticker.Stop()
	wasConnected := false
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			connected := inv.Connected()
			if wasConnected && !connected {
				proxy.InvalidateAll() // drop stale cache on disconnect
			}
			wasConnected = connected
			if connected {
				proxy.PollOnce(inv.ReadInput)
			}
		}
	}
}
