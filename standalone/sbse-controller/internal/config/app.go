package config

import (
	"os"
	"strings"
)

// App holds deployment-level settings that have no firmware analogue (the ESP32
// has fixed wiring for these). All come from environment variables so they map
// cleanly onto a k8s ConfigMap / Deployment env block.
type App struct {
	// HTTPAddr is the listen address for the REST API + dashboard, e.g. ":8080".
	HTTPAddr string
	// ConfigPath is where Config is persisted (replaces NVS). "" disables persistence.
	ConfigPath string
	// MQTT is the optional broker mirror.
	MQTT MQTT
}

// MQTT mirrors state/config to a broker and accepts command/update topics, the
// same surface the firmware's Mqtt module auto-publishes.
type MQTT struct {
	Enabled  bool
	Broker   string // e.g. tcp://broker:1883
	Prefix   string // topic prefix, e.g. "sbse/"
	ClientID string
	Username string
	Password string
}

// AppFromEnv builds App from the environment with sensible container defaults.
func AppFromEnv() App {
	a := App{
		HTTPAddr:   getenv("SBSE_HTTP_ADDR", ":8080"),
		ConfigPath: getenv("SBSE_CONFIG_PATH", "/data/config.json"),
		MQTT: MQTT{
			Enabled:  truthy(os.Getenv("SBSE_MQTT_ENABLED")),
			Broker:   os.Getenv("SBSE_MQTT_BROKER"),
			Prefix:   getenv("SBSE_MQTT_PREFIX", "sbse/"),
			ClientID: getenv("SBSE_MQTT_CLIENT_ID", "sbse-controller"),
			Username: os.Getenv("SBSE_MQTT_USERNAME"),
			Password: os.Getenv("SBSE_MQTT_PASSWORD"),
		},
	}
	// Normalise the prefix to end with a single trailing slash if non-empty.
	if a.MQTT.Prefix != "" && !strings.HasSuffix(a.MQTT.Prefix, "/") {
		a.MQTT.Prefix += "/"
	}
	return a
}

func getenv(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}
