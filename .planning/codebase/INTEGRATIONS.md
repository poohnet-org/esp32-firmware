# External Integrations

**Analysis Date:** 2026-03-12

## APIs & External Services

**Firmware Update Service:**
- Tinkerforge firmware repository
  - Updates URL: `https://www.warp-charger.com/firmwares/` (configurable in `software/warp_poohnet.ini`)
  - Latest URL: `https://www.warp-charger.com/downloads/#firmwares-warp1`
  - Implementation: `software/src/modules/firmware_update/`
  - Auto-rollback capability for failed updates

**Remote Access Service:**
- Service: Custom Tinkerforge remote access server
  - Server: `my.warp-charger.com` (configurable)
  - Protocol: WireGuard VPN tunneling
  - SDK/Client: WireGuard-ESP32-Arduino (https://github.com/Tinkerforge/WireGuard-ESP32-Arduino)
  - Implementation: `software/src/modules/remote_access/remote_access.h`
  - Features: HTTPS registration, connection management, ping monitoring

**Solar Forecast Service:**
- Integration with solar forecast data providers
  - Module: `software/src/modules/solar_forecast/`
  - Purpose: Day-ahead pricing and PV generation prediction

**Day-Ahead Pricing (DAP) Service:**
- Integration with electricity pricing APIs
  - Module: `software/src/modules/day_ahead_prices/`
  - Purpose: Dynamic charging based on market prices

## Data Storage

**Databases:**
- No traditional database
- Configuration/state persistence: NVS (Non-Volatile Storage) in ESP32 flash
- Charge tracking data: Optional SD card support
  - Client: `software/src/modules/em_sdcard/`, `software/src/modules/charge_tracker/`

**File Storage:**
- Internal ESP32 flash (NVS partition)
- Optional microSD card via SDMMC interface
- Firmware storage with coredump support (16MB partition table)
- SSL/TLS certificates stored in firmware or generated on-device via `software/src/modules/certs/`

**Caching:**
- In-memory message cache for MQTT topics (real-time state sync)
- WebSocket message caching for client reconnection recovery
- HTTP state caching in Python development server (`software/web/server.py`)

## Authentication & Identity

**Auth Provider:**
- Custom implementation with argon2 password hashing
  - Frontend: argon2-browser (client-side hashing in web UI)
  - Backend: Custom authentication module in `software/src/modules/authentication/`
  - Implementation: User credentials stored in config, argon2 hashing for security

**API Security:**
- Token-based authentication (authorization tokens stored in config)
- HTTPS/TLS for transport security
- Client certificate support for MQTT over TLS
- Custom authentication backend for HTTP API: `software/src/modules/http/http.h`

**NFC Card Integration:**
- NFC tag reading for user identification
  - Module: `software/src/modules/nfc/`
  - Bricklet: Tinkerforge NFC Bricklet (device ID 286)
  - Purpose: Contactless user authentication for charging

## Monitoring & Observability

**Error Tracking:**
- Coredump support for crash debugging
  - Module: `software/src/modules/coredump/`
  - Partition: Dedicated 16MB coredump partition for crash data

**Logs:**
- Event log system: `software/src/modules/event_log/`
- Debug logging with configurable levels
- MQTT publish for remote monitoring of events
- Python development server logs to console

**Debugging:**
- Debug protocol module: `software/src/modules/debug_protocol/`
- Debug module with admin-only access: `software/src/modules/debug/`
- Power cycle testing: `software/src/modules/power_cycle_tester/`
- RCT power client debug: `software/src/modules/rct_power_debug/`
- Modbus TCP debug: `software/src/modules/modbus_tcp_debug/`

## CI/CD & Deployment

**Hosting:**
- Embedded HTTP server in firmware: `software/src/modules/web_server/`
- Web interface served as standalone HTML/JS bundle
- PWA (Progressive Web App) support: `software/src/modules/pwa/`

**CI Pipeline:**
- PlatformIO-based build in GitHub Actions/Jenkins
- Configuration: `software/jenkins.ini` for CI builds
- Build artifacts: `.bin` firmware files, `.elf` debug symbols
- Firmware signing support with configurable presets (e.g., `poohnet:v1`)

## Environment Configuration

**Required env vars (Runtime):**
- Device hostname: Set via API or factory default (configurable prefix in .ini)
- WiFi SSID/Password: Configured via web UI or API
- MQTT broker address: Optional, configured in web UI
- NTP servers: Configurable via SNTP DHCP or manual settings
- Time zone: Configurable, with translation table in `software/src/modules/ntp/timezone_translation.h`

**Secrets location:**
- Configuration stored in ESP32 NVS (non-volatile storage), not in environment variables
- MQTT broker credentials: In config store
- TLS certificates: Generated on-device or embedded in firmware
- Private keys: WireGuard keys generated and stored in NVS during registration
- API tokens: Stored in device config

**Build-time configuration:**
- PlatformIO .ini files control firmware features and branding
- Custom options in platformio.ini define product behavior (e.g., WARP-specific settings)
- Signature presets in CI builds for signed firmware distribution

## Webhooks & Callbacks

**Incoming:**
- HTTP API endpoints for command execution: `software/src/modules/http/http.h`
- WebSocket real-time API: `software/src/modules/ws/`
- Automation triggers via HTTP: `software/src/modules/automation/`
- MQTT command subscriptions: `software/src/modules/mqtt/mqtt.h`

**Outgoing:**
- HTTP client for firmware updates: `software/src/modules/firmware_update/`
- HTTPS remote access registration: `software/src/modules/remote_access/`
- MQTT publish for state updates (retained messages)
- Solar forecast data polling
- Day-ahead price data fetching
- Event notifications via configured handlers

## Network Protocols

**WiFi:**
- IEEE 802.11 b/g/n (2.4GHz)
- Station (STA) and Access Point (AP) modes
- EAP authentication support (TLS, PEAP/TTLS)
- Module: `software/src/modules/wifi/`

**Ethernet:**
- Optional Ethernet support via Tinkerforge ESP32 Ethernet Brick
- RJ45 connector
- Module: `software/src/modules/ethernet/`, `software/src/modules/esp32_ethernet_brick/`

**MQTT:**
- Protocol: MQTT 3.1.1 / 5.0
- Broker connection: Configurable host, port, username/password
- TLS/SSL support with certificate pinning
- Auto-discovery for Home Assistant: `software/src/modules/mqtt_auto_discovery/`
- Message QoS, retained messages, topic subscriptions
- Module: `software/src/modules/mqtt/mqtt.h`
- Client library: `mqtt_client.h` (ESP-IDF)

**Modbus TCP:**
- Protocol: Modbus TCP (IEC 61588)
- Use cases: Meter communication, battery integration, inverter integration
- Server mode: `software/src/modules/modbus_tcp/`
- Client mode: `software/src/modules/modbus_tcp_client/`
- Integration: Victron, Varta, Sungrow, SolarMax, SolarEdge, SMA, Huawei, Growatt, etc.
- Virtual meter registers for standardized communication

**OCPP (Open Charge Point Protocol):**
- Protocol: OCPP 1.6 (with 2.0.1 extensions)
- Server: Central Charging System (CSMS)
- Implementation: TFOCPP library (submodule)
- Module: `software/src/modules/ocpp/ocpp.h`
- Features: Charge point registration, command handling, status reporting

**ISO 15118 (Plug & Charge):**
- Protocol: ISO 15118-2 / DIN 70121
- Use: Electric vehicle communication during charging
- Module: `software/src/modules/iso15118/`
- Meter integration: `software/src/modules/meters_iso15118/`

**EEBUS:**
- Protocol: EEBUS energy/information exchange
- Use: Smart grid device communication
- Module: `software/src/modules/eebus/`

**Modbus RTU (Serial):**
- Protocol: Modbus RTU over RS-485
- Hardware: Tinkerforge RS485 Bricklet
- Meter integration: `software/src/modules/meters_rs485_bricklet/`

**SMA Speedwire:**
- Protocol: SMA proprietary high-speed inverter communication
- Use: Direct SMA inverter integration
- Module: `software/src/modules/meters_sma_speedwire/`

**RCT Power (RCT/SunSpec):**
- Protocol: RCT proprietary + SunSpec compatibility
- Use: RCT inverter communication
- Module: `software/src/modules/meters_rct_power/`, `software/src/modules/rct_power_client/`

**SunSpec (ModbusTCP):**
- Protocol: SunSpec standard Modbus registers
- Use: Standardized inverter/meter communication
- Module: `software/src/modules/meters_sun_spec/`

**NTP:**
- Protocol: SNTP (Simple NTP)
- Servers: Configurable, with DHCP-based server discovery
- Module: `software/src/modules/ntp/ntp.h`
- Timezone support with DST handling

**DNS/mDNS:**
- mDNS hostname registration for local discovery
- Standard DNS for external resolution

**HTTPS/TLS:**
- Certificate generation: `software/src/modules/certs/`
- Server: Self-signed or custom certificates
- Client support for secure external API calls

**WebSocket:**
- Real-time bidirectional communication
- Module: `software/src/modules/ws/`
- Used for live state updates and command responses
- Python dev server proxy: `software/web/server.py`

## Integrations Summary

**Meter Types Supported:**
- Tinkerforge RS485 Bricklet (Modbus RTU)
- Modbus TCP (50+ device profiles: Victron, Varta, Sungrow, SMA, Huawei, Growatt, Fronius, SolarMax, SolarEdge, Fox ESS, Hailei, SAX Power, Shelly, Solax, SolarEdge, etc.)
- SMA Speedwire
- RCT Power
- SunSpec (Modbus TCP)
- MQTT mirror (subscribe to external MQTT topics as meter data)
- API meter (HTTP/REST polling)
- Legacy API (deprecated Tinkerforge API protocol)
- PV faker (testing/simulation)

**Battery Systems:**
- Modbus TCP battery integration
- Victron Energy
- Varta batteries (Element, Flex)
- Custom Modbus register mapping
- Module: `software/src/modules/batteries_modbus_tcp/`

**Inverter/PV Integration:**
- Direct Modbus TCP communication
- SMA Speedwire protocol
- RCT Power native protocol
- SunSpec standardization layer

**Remote Management:**
- Remote access via WireGuard VPN to `my.warp-charger.com`
- Firmware OTA updates from Tinkerforge servers
- Telemetry and monitoring

**Energy Trading/Optimization:**
- Solar forecast integration
- Day-ahead pricing feeds
- Charge manager coordination with multiple chargers

---

*Integration audit: 2026-03-12*
