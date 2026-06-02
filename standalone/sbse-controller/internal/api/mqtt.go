package api

import (
	"encoding/json"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/poohnet/sbse-controller/internal/config"
	"github.com/poohnet/sbse-controller/internal/control"
)

// MQTT mirrors the controller's state/config/active_config to a broker and accepts
// the same update/command topics the firmware's Mqtt module auto-publishes
// (<prefix>sbse_controller/...).
type MQTT struct {
	client mqtt.Client
	prefix string
	ctrl   *control.Controller
	logf   func(string, ...any)
}

// NewMQTT connects to the broker and subscribes to the command/update topics.
func NewMQTT(cfg config.MQTT, ctrl *control.Controller, logf func(string, ...any)) (*MQTT, error) {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	m := &MQTT{prefix: cfg.Prefix, ctrl: ctrl, logf: logf}

	opts := mqtt.NewClientOptions().
		AddBroker(cfg.Broker).
		SetClientID(cfg.ClientID).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectTimeout(10 * time.Second).
		SetOnConnectHandler(func(mqtt.Client) {
			m.logf("mqtt connected to %s", cfg.Broker)
			m.subscribe()
			// Republish current state on (re)connect.
			m.PublishConfig(ctrl.ConfigSnapshot())
			m.PublishActive(ctrl.ActiveSnapshot())
			m.PublishState(ctrl.Snapshot())
		})
	if cfg.Username != "" {
		opts.SetUsername(cfg.Username)
		opts.SetPassword(cfg.Password)
	}

	client := mqtt.NewClient(opts)
	if tok := client.Connect(); tok.Wait() && tok.Error() != nil {
		return nil, tok.Error()
	}
	m.client = client
	return m, nil
}

func (m *MQTT) topic(suffix string) string { return m.prefix + "sbse_controller/" + suffix }

func (m *MQTT) publish(suffix string, v any) {
	if m.client == nil {
		return
	}
	data, err := json.Marshal(v)
	if err != nil {
		return
	}
	m.client.Publish(m.topic(suffix), 0, true, data)
}

// PublishState / PublishConfig / PublishActive mirror the corresponding objects.
func (m *MQTT) PublishState(s control.State)        { m.publish("state", s) }
func (m *MQTT) PublishConfig(c config.Config)       { m.publish("config", c) }
func (m *MQTT) PublishActive(a config.ActiveConfig) { m.publish("active_config", a) }

func (m *MQTT) subscribe() {
	m.client.Subscribe(m.topic("config_update"), 0, func(_ mqtt.Client, msg mqtt.Message) {
		cur := m.ctrl.ConfigSnapshot()
		if err := json.Unmarshal(msg.Payload(), &cur); err != nil {
			m.logf("mqtt config_update parse: %v", err)
			return
		}
		if err := m.ctrl.UpdateConfig(cur); err != nil {
			m.logf("mqtt config_update rejected: %v", err)
		}
	})
	m.client.Subscribe(m.topic("active_config_update"), 0, func(_ mqtt.Client, msg mqtt.Message) {
		cur := m.ctrl.ActiveSnapshot()
		if err := json.Unmarshal(msg.Payload(), &cur); err != nil {
			m.logf("mqtt active_config_update parse: %v", err)
			return
		}
		if err := m.ctrl.UpdateActiveConfig(cur); err != nil {
			m.logf("mqtt active_config_update rejected: %v", err)
		}
	})
	m.client.Subscribe(m.topic("pause"), 0, func(_ mqtt.Client, _ mqtt.Message) {
		m.ctrl.Pause()
	})
	m.client.Subscribe(m.topic("resume"), 0, func(_ mqtt.Client, _ mqtt.Message) {
		m.ctrl.Resume()
	})
}

// Close disconnects from the broker.
func (m *MQTT) Close() {
	if m.client != nil {
		m.client.Disconnect(250)
		m.client = nil
	}
}
