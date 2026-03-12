# Architecture

**Analysis Date:** 2026-03-12

## Pattern Overview

**Overall:** Modular framework with plugin-style architecture

**Key Characteristics:**
- Module-based system where each feature is a self-contained, independently compilable module
- Event-driven pub/sub architecture with REST/WebSocket API layer
- Compile-time module selection via PlatformIO configuration
- Hardware abstraction through device modules that wrap hardware bindings
- Synchronous boot sequence with explicit lifecycle hooks

## Layers

**Hardware Abstraction Layer (HAL):**
- Purpose: Encapsulate all device-specific interactions with Tinkerforge bricklets and hardware
- Location: `software/src/bindings/` (auto-generated from `.c` files)
- Contains: Bricklet bindings (EVSE, meters, NFC, etc.)
- Depends on: TF_HAL (managed at `software/src/bindings/hal_common.h`)
- Used by: `DeviceModule` subclasses

**Module Interface Layer:**
- Purpose: Define the contract all modules must implement
- Location: `software/src/module.h`
- Contains: `IModule` abstract base class with lifecycle hooks
- Lifecycle hooks: `pre_init()` → `pre_setup()` → `setup()` → `register_urls()` → `register_events()` → `loop()` → `pre_reboot()`
- Depends on: Nothing (base abstraction)
- Used by: All module implementations

**Core Framework:**
- Purpose: Orchestrate module lifecycle, manage configuration, provide common services
- Location: `software/src/main.cpp`, `software/src/modules.cpp`, `software/src/modules.h`
- Contains: Module registry, boot orchestration, event loop management
- Depends on: All module declarations
- Used by: PlatformIO during compilation

**Module Implementations:**
- Purpose: Feature-specific business logic
- Location: `software/src/modules/{module_name}/`
- Contains: `.h` and `.cpp` files implementing `IModule` interface
- Pattern: Most modules are singletons created at global scope
- Depends on: Core framework, HAL, config system, API backend
- Used by: Main loop, event handlers, other modules (via API/state updates)

**Device Module Pattern:**
- Purpose: Wrapper around hardware devices that standardizes initialization and firmware updates
- Location: Base at `software/src/device_module.h`, implementations at `software/src/modules/{device}/`
- Template class: `DeviceModule<DeviceT, init_fn, bootloader_mode_fn, reset_fn, destroy_fn>`
- Handles: Device discovery, firmware flashing, identity tracking
- Examples: `EVSE`, `RtcBricklet`, `NFC`, `Meters`

**Configuration System:**
- Purpose: Type-safe, hierarchical config storage with validation
- Location: `software/src/config.h`, `software/src/config.cpp`
- Contains: Config variant types (String, Int, Array, Object, Union, etc.)
- Supports: File persistence, JSON serialization, config validation
- Used by: Modules to declare and manage configuration state
- Migration path: `software/src/config_migrations.cpp`

**API Backend System:**
- Purpose: Expose module state/commands to clients (web, external APIs)
- Location: `software/src/modules/api/api.h`
- Pattern: Modules register states (read-only data) and commands (RPC calls)
- Transport: HTTP REST and WebSocket (via `software/src/modules/ws/`)
- State updates: Pub/sub with per-state subscription capability
- Used by: Web frontend, external integrations (MQTT, Modbus)

**Web Server & HTTP:**
- Purpose: Serve web interface and REST API
- Location: `software/src/modules/web_server/`, `software/src/modules/http/`
- Frameworks: ESP-IDF HTTP daemon
- Authentication: Digest auth via `software/src/digest_auth.cpp`
- Assets: Embedded gzipped HTML/CSS/JS from `software/web/` (template processing)

**Event System:**
- Purpose: Asynchronous notifications when system state changes
- Location: `software/src/modules/event/`, `software/src/modules/event_log/`
- Pattern: Modules push events to event log, clients subscribe via WebSocket
- Usage: Status updates, errors, configuration changes

## Data Flow

**Initialization Sequence:**

1. **pre_init()** - For modules requiring earliest possible setup (logging, HAL)
2. **pre_setup()** - Mount filesystems, load persisted config
3. **setup()** - Initialize hardware, register config handlers
4. **register_urls()** - Add HTTP endpoints
5. **register_events()** - Set up event subscriptions
6. **Main loop()**  - Round-robin execution of modules with loop() overrides

**Configuration Update Flow:**

1. Client sends POST to `/api/config/{path}` with JSON payload
2. HTTP module routes to API backend
3. API validates and calls `set()` on target Config object
4. Config handler triggers registered update callbacks
5. Module validates change, updates internal state
6. State update propagates to WebSocket subscribers
7. Persistence layer writes to SPIFFS

**Event Publishing:**

1. Module calls `event.push_event()` with event data
2. Event log stores in circular buffer
3. WebSocket connections receive incremental updates
4. Clients can retroactively fetch event history

**State Query:**

1. Client sends GET to `/api/config/{path}`
2. API backend serializes state to JSON via Config::to_cstring()
3. HTTP response includes state value
4. Response can be chunked for large payloads

**State Management:**

- Modules declare mutable state as `ConfigRoot` objects
- Config system provides write-once semantics with validation
- Persistence handled transparently to SPIFFS
- HAL device state synchronized via poll/interrupt handlers in module loop()

## Key Abstractions

**IModule Interface:**
- Purpose: Standard lifecycle for all features
- Examples: `API`, `WebServer`, `Meters`, `EVSE`, `Automation`
- Pattern: Modules override only hooks they need

**IAPIBackend:**
- Purpose: Allow multiple transport backends (HTTP, MQTT, future protocols)
- Examples: `software/src/modules/http/http.h` (REST), `software/src/modules/mqtt/mqtt.h`
- Pattern: Modules register states/commands once, backend handles exposure

**IEvseBackend, IBatteryGenerator, etc.:**
- Purpose: Abstraction for pluggable implementations of core functionality
- Location: `software/src/modules/{capability}_common/`
- Pattern: Common module defines interface, specific implementations satisfy it
- Example: `EvseCommon` aggregates multiple EVSE backend implementations

**DeviceModule Template:**
- Purpose: Boilerplate reduction for hardware device wrappers
- Pattern: Template specialization on device type + function pointers
- Reduces: Repetitive init/destroy/reset code across bricklet modules

**ConfigRoot:**
- Purpose: Strongly-typed hierarchical state container
- Pattern: Variant-based, supports nested objects/arrays
- Operations: read, write (with validation), subscribe to changes, serialize to JSON

## Entry Points

**main() / setup() / loop():**
- Location: `software/src/main.cpp`
- Triggers: Arduino runtime (ESP-IDF)
- Responsibilities:
  - Boot all modules through lifecycle hooks
  - Initialize task scheduler and HAL
  - Register default API endpoints (`/`, `/reboot`, `/login_state`)
  - Set up watchdog and reboot handler
  - Execute modules' loop() functions in round-robin

**HTTP Request Entry Points:**
- Path: Any endpoint registered via `server.on_HTTPThread()` or `api.addCommand()`
- Triggers: HTTP client (web browser, API consumer)
- Responsibilities: Route to handler, authenticate, call module logic, serialize response

**WebSocket Entry Points:**
- Location: `software/src/modules/ws/`
- Triggers: WebSocket client connection
- Responsibilities: Subscribe to state updates, forward events, handle subscriptions

**Task Scheduler:**
- Location: `software/src/modules/task_scheduler/`
- Triggers: Delayed tasks, repeating tasks
- Pattern: Async execution without spawning threads

## Error Handling

**Strategy:** Fail gracefully with logging, continue operation where possible

**Patterns:**
- Hardware errors: Log to event log, continue (optional device)
- Config validation: Reject invalid changes, return error to client
- Network errors: Retry with exponential backoff
- Watchdog timeout: Trigger reboot with diagnostic information
- Exceptions: Caught at HTTP/WebSocket boundaries, return 500 errors

## Cross-Cutting Concerns

**Logging:**
- Framework: `logger` global singleton (`software/src/modules/event_log/`)
- Pattern: `logger.printfln()` for messages, stored in circular buffer
- Level: Single level (no severity filtering)

**Validation:**
- Location: `software/src/config.h` - type system enforces valid ranges
- Pattern: Config objects specify min/max, type for ints; regex for strings
- API: Validation happens on config write, errors propagated to client

**Authentication:**
- Scheme: HTTP Digest Auth (RFC 2617)
- Location: `software/src/digest_auth.cpp`
- Storage: Username/password hash persisted in config
- Pattern: Checked before route handlers execute

---

*Architecture analysis: 2026-03-12*
