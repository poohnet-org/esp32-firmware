# Technology Stack

**Analysis Date:** 2026-03-12

## Languages

**Primary:**
- C++ (C++17/20) - Core firmware for ESP32 embedded system, located in `software/src/`
- C - Arduino framework and bindings in `software/src/bindings/`

**Secondary:**
- Python 3 - Web backend and build utilities (Flask development server, PlatformIO scripts)
- TypeScript/JavaScript - Web frontend UI located in `software/web/src/`
- HTML/SCSS - Frontend styling and markup

## Runtime

**Environment:**
- ESP32 (Espressif) microcontroller with custom Tinkerforge firmware variants
- Arduino framework with Tinkerforge customizations (`platform-espressif32`)
- PSRAM available on ethernet-enabled variants

**Package Manager:**
- PlatformIO - Primary build/dependency manager for embedded firmware
- npm - Package manager for web frontend (`software/web/package.json`)
- pip - Python dependency manager (`software/requirements.txt`)

**Lockfile:**
- PlatformIO: uses `.pio/` cached libraries and package management
- npm: package-lock.json present in web directory
- pip: requirements.txt with pinned packages (platformio, pyphen, tinkerforge, flask, flask_sock, websocket, Xlsx2csv)

## Frameworks

**Core:**
- Arduino ESP32 Framework (Tinkerforge-patched version: `tf-3-3-7`) - Base microcontroller abstraction
- ESP-IDF components - Used via Arduino framework for FreeRTOS, networking, peripherals

**Frontend:**
- Preact 10.28.3 - Lightweight React alternative for UI rendering
- React-Bootstrap 2.10.10 - Bootstrap 5 components for React/Preact
- uplot 1.6.30 - Charting library for energy/power graphs
- Bootstrap 5.3.8 - CSS framework for responsive design

**Backend Web Server:**
- Flask - Lightweight Python web framework for development server
- Flask-Sock - WebSocket support for Flask
- Async HTTPS Client - Custom C++ HTTP client for remote access and integrations

**Development/Build:**
- PlatformIO - Cross-platform embedded development environment
- esbuild 0.27.3 - Fast JavaScript bundler and minifier
- Sass 1.97.3 - CSS preprocessing
- TypeScript 5.9.3 - Static typing for JavaScript
- PostCSS - CSS transformation with autoprefixer and cssnano

## Key Dependencies

**Critical (C/C++):**
- ArduinoJson (Tinkerforge fork) - JSON parsing/serialization for device communication
- strict_variant - Type-safe variant type for C++
- tfjson (Tinkerforge) - Custom JSON library optimized for embedded
- tftools (Tinkerforge) - Embedded development utilities
- tfnetwork (Tinkerforge) - Network abstraction layer
- WireGuard-ESP32-Arduino - VPN capability for remote access
- OCPP library (in submodule `.pio/libdeps/warp2_poohnet/TFOCPP/`) - Open Charge Point Protocol implementation

**Infrastructure:**
- TFJson/ArduinoJson - Message serialization and API communication
- mqtt_client.h (ESP-IDF) - MQTT protocol client
- esp_http_server.h (ESP-IDF) - Embedded HTTP/HTTPS server
- WebSocket support via WS module - For real-time state updates
- Modbus TCP - Protocol implementation for meter communication
- mDNS/DNS - Service discovery
- SNTP - Network time protocol for clock synchronization

**Frontend (Node.js):**
- @preact/signals 2.7.1 - Reactive state management
- deepsignal 1.6.0 - Deep signal state tracking
- argon2-browser 1.18.0 - Client-side password hashing
- react-feather 2.0.10 - SVG icon library
- yamd5.js 1.0.0 - MD5 hashing utility

**Development (Node.js):**
- autoprefixer 10.4.24 - CSS vendor prefixing
- cssnano 7.1.2 - CSS minification
- html-minifier-terser 7.2.0 - HTML minification
- median-js-bridge 2.13.4 - Build tooling integration

## Configuration

**Environment:**
- Configured via `platformio.ini` build configurations for different boards/products
- Multiple environment profiles: `warp_poohnet`, `warp_poohnet_debug`, `energy_manager_v2`, etc.
- Build flags defined in platformio.ini with compiler optimizations (-Os, -Wall, -Wextra)
- Custom build defines: `TF_NET_ENABLE=1`, `SNTP_GET_SERVERS_FROM_DHCP=1`, `ARDUINOJSON_USE_DOUBLE=1`

**Build:**
- `platformio.ini` - Main build configuration with board definitions, library dependencies, platform packages
- Pre-build hooks: `pip_install.py`, `pio_hooks.py` for setup
- Post-build hooks: `merge_firmware_hook.py`, `fast_reflash_upload.py` for firmware processing
- Board partitions: 16MB coredump partition tables for ESP32, 4MB variants available
- Custom modules injections via `custom_backend_modules_injections`, `custom_frontend_modules_injections`

**Development Server:**
- Flask development server in `software/web/server.py` forwards requests to ESP32 device
- Proxies HTTP/WebSocket traffic to target device (configurable host)
- Runs on IPv6 localhost with auto-port-selection (default port 5000)

## Platform Requirements

**Development:**
- Python 3.6+ (for PlatformIO and build scripts)
- Node.js 14+ (for npm web frontend building)
- PlatformIO IDE or CLI
- USB connection to ESP32 for flashing (serial via TTL-USB adapter)
- Tinkerforge platform package with custom Arduino framework patches

**Production:**
- ESP32 microcontroller with 4MB or 16MB flash
- 8MB PSRAM (optional, for ethernet variant)
- Network connectivity via WiFi (2.4GHz 802.11 b/g/n) or Ethernet (RJ45)
- Optional Tinkerforge Bricklets for: RS485, NFC, RTC 2.0, temperature sensors
- SSL/TLS certificates for HTTPS (embedded in firmware or generated on device)

## Memory & Performance

**ESP32 Constraints:**
- 520KB SRAM + optional PSRAM up to 8MB
- 4MB or 16MB flash storage
- Dual-core processor @ ~240MHz
- Optimized with `-Os` compiler flags
- Network events use MUTEX protection for thread safety
- ArduinoJson uses double precision floats for accuracy

**Firmware Size:**
- Multiple configuration options affect build size (debug vs. release, module inclusions)
- Coredump support enabled in partition scheme for crash debugging
- Firmware update capability with rollback support

---

*Stack analysis: 2026-03-12*
