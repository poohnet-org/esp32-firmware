# Testing Patterns

**Analysis Date:** 2026-03-12

## Test Framework

**Runner:**
- Embedded system: No traditional unit test framework for C++ firmware code
- Python provisioning scripts: `unittest` / custom test harnesses
- Integration: Hardware-in-loop testing via physical ESP32 devices

**Assertion Library:**
- None detected for C++ embedded code
- Python: Standard `assert` statements and exception handling

**Run Commands:**
- PlatformIO builds: `pio run -e <env>` (see `software/platformio.ini`)
- Python scripts: Direct execution with `python <script.py>`
- Tests are primarily hardware-based due to embedded nature

## Test File Organization

**Location:**
- Embedded C++ tests: Not co-located; testing done via hardware integration
- Provisioning Python tests: Located in `provisioning/tinkerforge/` directory
  - Example: `provisioning/tinkerforge/bricklet_common_test.py`
  - Example: `provisioning/tinkerforge/bricklet_stream_test.py`

**Naming:**
- Python test files: `*_test.py` suffix
- Test methods: `test_*` prefix (Python convention)
- Hardware/integration tests: Device provisioning and flashing scripts (stage-based approach)

**Structure:**
```
provisioning/
├── provision_stage_0_warp1.py
├── provision_stage_1_esp32.py
├── provision_stage_2_energy_manager.py
├── provision_stage_3_warp2.py
├── tinkerforge/
│   ├── bricklet_common_test.py
│   ├── bricklet_stream_test.py
│   └── [device binding modules]
└── provision_common/
    └── [common test utilities]
```

## Test Structure

**Python Test Pattern:**
Tests use direct assertions and exception handling. Example from provisioning utilities:

```python
# From inventory.py - change tracking test
def _report_change(self, change, entry):
    if self._change_callback != None:
        try:
            self._change_callback(change, entry)
        except:
            pass  # FIXME - error handling needed
```

**Patterns:**
- Context managers for test isolation: `with ChangedDirectory('/path')` and `with temp_file()`
- Setup via helper functions: `common_init(port)` initializes global state
- Teardown via context manager `__exit__` methods

**Example Test Utilities** (from `provision_common.py`):
```python
@contextmanager
def temp_file():
    fd, name = tempfile.mkstemp()
    try:
        yield fd, name
    finally:
        try:
            os.remove(name)
        except IOError:
            print('Failed to clean up temp file {}'.format(name))

class ChangedDirectory:
    def __init__(self, path):
        self.path = path
        self.previous_path = None

    def __enter__(self):
        self.previous_path = os.getcwd()
        os.chdir(self.path)

    def __exit__(self, type_, value, traceback):
        os.chdir(self.previous_path)
```

## Mocking

**Framework:**
- No explicit mocking library detected for Python tests
- Manual stub implementations via simulation modules

**Patterns:**
- Simulator modules: `provisioning/provision_common/sdm_simulator.py`
  - Simulates meter behavior for testing without physical hardware
  - Used in provisioning stage tests

**What to Mock:**
- Hardware devices: Use simulator modules when testing provisioning logic
- Serial connections: Via esptool/espefuse wrappers
- Network communication: Via test helper functions

**What NOT to Mock:**
- Device enumeration and discovery: Real IPConnection required
- Configuration persistence: Real flash storage integration
- Hardware state machines: Requires device simulation or real hardware

## Fixtures and Factories

**Test Data:**
Python provisioning uses namedtuple for structured test data:

```python
# From inventory.py
InventoryEntry = namedtuple('InventoryEntry',
    'uid connected_uid position hardware_version firmware_version device_identifier')
```

Dynamic fixture creation from configuration:
```python
# From batteries.cpp - config prototype factory
ConfUnionPrototype<BatteryClassID> *config_prototypes =
    new ConfUnionPrototype<BatteryClassID>[class_count];
```

**Location:**
- Python fixtures: In-line within test scripts (`provisioning/provision_*.py`)
- C++ test data: Generated from config schemas (`software/src/config/`)
- Simulator implementations: `provisioning/provision_common/sdm_simulator.py`

## Coverage

**Requirements:**
- No explicit code coverage enforcement detected
- Implicit coverage: All modules must compile and initialize successfully
- Integration coverage: Each provisioning stage validates device functionality

**View Coverage:**
- Compiler warnings: `-Wall -Wextra` enforces most issues caught at build time
- Hardware validation: Device functionality verified during provisioning stages

## Test Types

**Unit Tests:**
- Scope: Config validation, parsing, migration logic
- Approach: Inline validation functions in modules (return error strings)
- Example: Authentication config validator in `authentication.cpp`:
  ```cpp
  [this](Config &update, ConfigSource source) -> String {
      if (update.get("enable_auth")->asBool() &&
          update.get("digest_hash")->asString().isEmpty())
          return "Authentication can not be enabled if no password is set.";
      return "";
  }
  ```

**Integration Tests:**
- Scope: Device provisioning, module initialization, API functionality
- Approach: Hardware-in-loop via provisioning scripts
- Stages:
  1. Stage 0: Initial device setup (device identification)
  2. Stage 1: ESP32 flashing and validation
  3. Stage 2: Energy manager configuration
  4. Stage 3: Advanced features (WARP2 specific)
- Files: `provisioning/provision_stage_*.py`

**E2E Tests:**
- Framework: Not used in embedded context
- Alternative: Full device provisioning pipeline validates end-to-end functionality
- Integration tests serve as E2E in this embedded environment

## Common Patterns

**Async/Await Testing:**
- Not applicable: Embedded C++ uses event callbacks and task scheduler
- Validation: Callback functions tested indirectly through config registration

**Error Testing:**
Pattern: Return error message strings from validators

```cpp
// From authentication.cpp - error testing pattern
String validate_result = config_validator(test_config, ConfigSource::API);
// Check: if (validate_result.isEmpty()) { /* valid */ }
//        else { /* invalid, error = validate_result */ }
```

**Test Isolation:**
- Python: Context managers and temporary files
- C++: Module lifecycle (pre_setup → setup → teardown) provides isolation
- State reset: Via `config.restorePersistentConfig()` loading from flash

**Typical Provisioning Test Flow:**
```python
# From provision_stage_1_esp32.py pattern
def provision_stage_1():
    # 1. Connect to device
    # 2. Validate existing firmware
    # 3. Flash new firmware
    # 4. Verify boot
    # 5. Restore configuration
    # 6. Validate functionality
```

## Build Validation

**Compiler as Test:**
- PlatformIO configuration enforces strict compilation:
  - `build_src_flags = -Os -Wall -Wextra -Werror=return-type -Werror=format`
  - All warnings are errors for return types and format strings
- Linker validation: All symbols must resolve
- Generated code validation: Type-checking on enums and configs

**Configuration Schema Validation:**
- Compile-time: C++ type checking on Config objects
- Runtime: Config validators called before state updates
- Persistence: Flash storage validates format on load

---

*Testing analysis: 2026-03-12*
