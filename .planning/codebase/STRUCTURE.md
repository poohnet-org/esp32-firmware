# Codebase Structure

**Analysis Date:** 2026-03-12

## Directory Layout

```
esp32-firmware/
├── provisioning/          # Mass provisioning scripts for ESP32 modules
├── software/              # Main firmware source code
│   ├── src/              # C++ firmware source
│   │   ├── modules/      # Feature modules (63+ modules)
│   │   ├── config/       # Configuration system implementation
│   │   ├── bindings/     # Hardware bindings (Tinkerforge bricklets)
│   │   ├── tools/        # Utility libraries
│   │   ├── net_arduino_esp32/  # Network abstraction
│   │   ├── main.cpp      # Entry point & boot orchestration
│   │   ├── modules.cpp   # Module registry (generated)
│   │   ├── module.h      # IModule interface
│   │   ├── config.h      # Config system types
│   │   └── *.cpp/*.h     # Core framework files
│   ├── web/              # TypeScript/React web frontend
│   │   ├── src/
│   │   │   ├── modules/  # Frontend module implementations
│   │   │   ├── ts/       # TypeScript utilities
│   │   │   ├── main.tsx  # React root component
│   │   │   ├── app.tsx   # App shell template
│   │   │   └── index.html
│   │   ├── tfpp.py       # Template preprocessor for code generation
│   │   ├── package.json  # Frontend dependencies
│   │   └── src_tfpp/     # Template fragments for code generation
│   ├── boards/           # Board-specific configurations
│   ├── best_practices/   # Documentation & patterns
│   ├── lib-builder/      # Custom build infrastructure
│   ├── *.ini             # PlatformIO environment configs
│   └── build/            # Compiled output (gitignored)
├── .github/              # GitHub Actions CI/CD
├── .planning/            # GSD planning documents (this directory)
└── README.rst
```

## Directory Purposes

**software/src/modules/:**
- Purpose: Feature implementations; each module is a self-contained feature
- Contains: 60+ module directories, each with `.cpp`/`.h` implementation
- Key structure per module: `module_name/` containing:
  - `{module_name}.h` - Interface/class declaration
  - `{module_name}.cpp` - Implementation
  - `generated/` - Auto-generated code (module registration, config schemas)
  - `tests/` - Optional unit tests (e.g., `automation/tests/`)
  - `module.ini` - Frontend module configuration (if has web UI)
  - `api.ts.template` - Frontend API bindings template

**software/src/config/:**
- Purpose: Configuration system implementation details
- Contains: Type implementations for Config variants (strings, ints, arrays, objects, unions)
- Key files:
  - `visitors.h` - Visitor pattern for config traversal
  - `slot_allocator.h/cpp` - Memory management for config storage
  - `prototypes.cpp` - Built-in type definitions

**software/src/bindings/:**
- Purpose: Hardware abstraction for Tinkerforge bricklets
- Contains: Auto-generated C headers from official Tinkerforge repositories
- Examples: `bricklet_evse.h`, `bricklet_energy_monitor.h`, `bricklet_gps_v3.h`
- Updated: Via dependency management in PlatformIO

**software/src/tools/:**
- Purpose: Shared utility libraries and helpers
- Contains: Memory management, filesystem utilities, DNS, allocator, backtrace
- Key files:
  - `allocator.h` - Custom memory allocator
  - `fs.cpp/h` - Filesystem operations
  - `arena.cpp/h` - Arena allocator for temporary objects
  - `freertos.h` - FreeRTOS task utilities

**software/src/net_arduino_esp32/:**
- Purpose: Network abstraction layer for ESP32 platform
- Contains: WiFi, Ethernet, mDNS implementations
- Pattern: Implements standard network interfaces on Arduino/ESP32 APIs

**software/web/src/modules/:**
- Purpose: Frontend components for each feature module
- Contains: TypeScript/React components organized by module name
- Structure per module:
  - `api.ts` - REST API client definitions (generated or manual)
  - `main.tsx` - Main React component
  - `plugins.tsx` - Plugin system for extensibility
  - `translation_de.tsx`, `translation_en.tsx` - Localization
  - `types.ts` - TypeScript interfaces
  - `.gitignore` - Prevent committing generated files

**software/web/src/ts/:**
- Purpose: TypeScript utilities and shared components
- Contains: API client base, form components, helpers
- Used by: All frontend modules

**software/web/src_tfpp/:**
- Purpose: Template source files for code generation
- Contains: .template files that `tfpp.py` processes
- Generated: `app.tsx`, `api_defs.ts`, `branding.ts` during build

## Key File Locations

**Entry Points:**

- `software/src/main.cpp` - Arduino setup()/loop() entry point; boot orchestration
- `software/src/modules.cpp` - Module registry; generated array of all compiled modules
- `software/web/src/main.tsx` - React root for web frontend
- `software/web/index.html.template` - HTML shell template

**Configuration:**

- `software/*.ini` - PlatformIO environment configs (e.g., `esp32.ini`, `energy_manager_v2.ini`)
- `software/src/options.h` - Compile-time feature flags
- `software/boards/` - Board-specific pin definitions

**Core Logic:**

- `software/src/module.h` - IModule interface definition
- `software/src/config.h` - Config system type definitions
- `software/src/device_module.h` - DeviceModule template for hardware wrappers
- `software/src/modules/api/api.h` - API backend interface

**HTTP Handling:**

- `software/src/modules/web_server/web_server.h` - HTTP server setup
- `software/src/modules/http/http.h` - REST API endpoint routing
- `software/src/modules/ws/ws.h` - WebSocket handling

**Testing:**

- `software/src/modules/automation/tests/` - Example test directory

**Build Configuration:**

- `software/build_all.py` - Multi-target build script
- `software/lib-builder/` - Custom compilation infrastructure
- `software/patches/` - Patches applied to dependencies

## Naming Conventions

**Files:**

- Module implementation: `{module_name}.h` / `{module_name}.cpp`
- Generated files: Suffix `_generated`, or in `generated/` subdirectory
- Headers: `.h` extension
- Source: `.cpp` extension
- Templates: `.template` suffix (e.g., `api.ts.template`)
- Config: `.ini` extension (PlatformIO)

**Directories:**

- Module folders: lowercase, underscore-separated (`phase_switcher`, `batteries_modbus_tcp`)
- Features grouped by domain: `evse*`, `meters*`, `batteries*`, `em_*` (energy manager specific)
- Auto-generated subdirs: `generated/`
- Architecture-specific: `net_arduino_esp32/`

**Classes/Types:**

- IModule implementations: PascalCase, no prefix (e.g., `EVSE`, `Automation`)
- Interfaces: `I` prefix + PascalCase (e.g., `IModule`, `IAPIBackend`)
- Config types: `Conf` + type name (e.g., `ConfString`, `ConfObject`)

**Functions:**

- Snake_case: `register_urls()`, `pre_setup()`
- Global singletons: `{module_name}_imodule` pointer

**Variables:**

- Member vars: Snake_case
- Global configs: `modules` (ConfigRoot)
- Global HAL: `hal` (TF_HAL)

## Where to Add New Code

**New Feature Module:**

1. Create: `software/src/modules/{feature_name}/`
2. Implementation:
   - `software/src/modules/{feature_name}/{feature_name}.h` - Declare class extending `IModule`
   - `software/src/modules/{feature_name}/{feature_name}.cpp` - Implement lifecycle hooks
   - `software/src/modules/{feature_name}/generated/module.cpp` - Auto-register with module system (or create manually initially)
3. Register: Add to appropriate `*.ini` file's `custom_backend_modules` list
4. Frontend (optional):
   - `software/web/src/modules/{feature_name}/main.tsx` - React component
   - `software/web/src/modules/{feature_name}/api.ts` - API definitions
   - Add to `custom_frontend_modules` in `.ini` file

**New Hardware Device Module:**

1. Create: `software/src/modules/{device_name}/`
2. Extend: `DeviceModule<BrickletType, init_fn, bootloader_fn, reset_fn, destroy_fn>`
3. Implement:
   - Hardware discovery in `pre_setup()`
   - API command/state registration in `setup()`
   - Periodic polling in `loop()` if needed

**New Utility Library:**

1. Create: `software/src/tools/{utility_name}.h` + `.cpp`
2. Include in modules via `#include "tools/{utility_name}.h"`
3. No external registration needed; header-only for inline utilities

**Frontend Component:**

1. Create: `software/web/src/modules/{module_name}/component.tsx`
2. Import in: `software/web/src/modules/{module_name}/main.tsx`
3. Use Bootstrap classes for styling (via `main.scss`)
4. Add translations to `translation_de.tsx` / `translation_en.tsx`

**Configuration Schema:**

1. Declare in module's `.cpp` file using `Config::Object()`, `Config::String()`, etc.
2. Register with API: `api.addState("path/to/config", &config_root)`
3. Optionally add validation via config handler callbacks

## Special Directories

**software/generated/:**
- Purpose: Build output directory (created during compilation)
- Generated: Embedded HTML/CSS/JS, web module registrations
- Committed: No (in `.gitignore`)

**software/build/:**
- Purpose: Compiled firmware binaries
- Generated: `.bin` files for flashing
- Committed: No

**.pio/:**
- Purpose: PlatformIO dependency cache
- Contents: Downloaded libraries, compiler toolchains
- Committed: No (in `.gitignore`)

**software/boards/:**
- Purpose: Board-specific configurations
- Contents: Pin definitions, clock settings, partition tables
- Committed: Yes

---

*Structure analysis: 2026-03-12*
