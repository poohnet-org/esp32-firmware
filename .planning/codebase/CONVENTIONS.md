# Coding Conventions

**Analysis Date:** 2026-03-12

## Naming Patterns

**Files:**
- C++ headers: `.h` extension (e.g., `async_https_client.h`, `authentication.h`)
- C++ implementation: `.cpp` extension (e.g., `async_https_client.cpp`, `authentication.cpp`)
- Python: `.py` extension (e.g., `provision_common.py`, `inventory.py`)
- Enums: `snake_case` with `.enum.h` suffix for generated files (e.g., `battery_class_id.enum.h`)
- Generated files: `generated/` directory prefix in filenames (e.g., `module_available.h`)

**Functions:**
- C++: `snake_case` for both member functions and free functions
  - Private/internal helpers: prefix with underscore (e.g., `_cb_connected`, `_report_change`)
  - Event handlers: suffix with `_handler` or `_callback` (e.g., `event_handler`, `onAuthenticate_HTTPThread`)
- Python: `snake_case` (e.g., `_cb_connected`, `_report_change`, `get_all`)

**Variables:**
- C++: `snake_case` throughout
  - Member variables: regular `snake_case` (no prefix/suffix convention observed)
  - Local variables: `snake_case`
  - Constants: `UPPER_CASE` with `constexpr` or `static constexpr`
- Python: `snake_case` for normal variables, `_prefixed` for internal/private (e.g., `_ipcon`, `_entires`, `_change_callback`)

**Types:**
- C++ classes: `PascalCase` (e.g., `AsyncHTTPSClient`, `Authentication`, `Batteries`)
- C++ enums: `PascalCase` with `enum class` (e.g., `AsyncHTTPSClientState`, `PathType`, `BatteryClassID`)
- C++ structs: `PascalCase` with `struct` keyword (e.g., `StateRegistration`, `CommandRegistration`, `AuthFields`)
- Macro constants: `UPPER_CASE` (e.g., `ASYNC_HTTPS_CLIENT_TIMEOUT`, `ASSERT_MAIN_THREAD`)

## Code Style

**Formatting:**
- Tool: `clang-format` configured at `.clang-format`
- Indent: 4 spaces, no tabs (`UseTab: Never`)
- Column limit: 520 characters (very permissive for long conditionals)
- Line endings: Unix LF only (`UseCRLF: false`)
- Brace style: Linux style (opening brace on same line for control structures)

**Clang-format Key Settings:**
- `AllowAllArgumentsOnNextLine: false` - All function arguments must fit on one line or each on separate lines
- `AllowAllParametersOfDeclarationOnNextLine: false` - All parameters must fit on one line or each on separate lines
- `BinPackArguments: false` - Don't pack multiple arguments on same line
- `BinPackParameters: false` - Don't pack multiple parameters on same line
- `BreakBeforeBinaryOperators: NonAssignment` - Break before binary operators except assignment
- `AllowShortFunctionsOnASingleLine: None` - Never collapse function definitions to one line
- `BreakConstructorInitializers: AfterColon` - Put initializer lists on new lines after colon

**Linting:**
- Compiler flags: `-Wall -Wextra -Wshadow=local -Werror=return-type -Werror=format`
- All warnings are errors for return type mismatches and format strings
- Shadow warnings enabled for local variables only
- Optimization: `-Os` (size optimization)

## Import Organization

**C++ Include Order:**
1. Project-specific headers (relative paths or quotes)
2. Standard library headers (`<vector>`, `<functional>`, etc.)
3. Arduino/Framework headers (`<WString.h>`, `<Arduino.h>`)
4. Third-party library headers (`<ArduinoJson.h>`, `<TFTools/Micros.h>`)
5. Generated headers (`generated/module_dependencies.h`)

**Pattern Example** (from `async_https_client.cpp`):
```cpp
#include "async_https_client.h"     // Project header first
#include "event_log_prefix.h"
#include "main_dependencies.h"
#include "build.h"
#include "options.h"
```

**Python Import Order:**
1. Standard library imports grouped
2. Local package imports with relative paths
3. Constants/utilities defined after imports

## Error Handling

**Return Value Patterns:**
- Boolean returns: `false` for failure, `true` for success
- String returns: `""` (empty string) for error/no error
- Pointer returns: `nullptr` for error/not found
- Void functions: Use logging for errors (see Logging section)

**Error Callback Pattern:**
- Many async operations use event-based error reporting with callback functions
- Error types defined as enums (e.g., `AsyncHTTPSClientError` with variants like `NoHTTPSURL`, `Timeout`, `HTTPError`)
- Union types in event structures to store error-specific data

**Example** (from `async_https_client.h`):
```cpp
enum class AsyncHTTPSClientError {
    NoHTTPSURL,
    Busy,
    NoCert,
    Timeout,
    HTTPClientInitFailed,
    // ... more error types
};

struct AsyncHTTPSClientEvent {
    AsyncHTTPSClientEventType type;
    union {
        struct {
            AsyncHTTPSClientError error;
            esp_tls_error_handle_t error_handle;
            // ... error-specific fields
        };
        // ... other event types
    };
};
```

**Validation Pattern:**
- Config validators return `String` - empty string means valid, non-empty is error message
- Validation occurs before state changes
- Example from `authentication.cpp`:
```cpp
[this](Config &update, ConfigSource source) -> String {
    if (update.get("enable_auth")->asBool() &&
        update.get("digest_hash")->asString().isEmpty())
        return "Authentication can not be enabled if no password is set.";
    return "";
}
```

## Logging

**Framework:** Custom logger using `logger` global object with `EVENT_LOG_PREFIX` macro

**Macro Definition Pattern:**
- First line of each `.cpp` file: `#define EVENT_LOG_PREFIX "identifier"`
- Prefix length: 15 characters max, shortened to fit (e.g., `"async_https_clnt"` instead of full name)
- Prefix included in log output for easy filtering

**Logging Functions:**
- `logger.printfln(...)` - Standard line-based logging
- `logger.printfln_battery(...)` - Module-specific variant
- Uses printf-style formatting

**Example** (from `async_https_client.cpp`):
```cpp
#define EVENT_LOG_PREFIX "async_https_clnt"

// Later in code:
logger.printfln("event_handler received HTTP_EVENT_ERROR with unexpected data: %iB @ %p",
                event->data_len, event->data);
```

## Comments

**When to Comment:**
- Header files: Minimal inline comments; structure definitions have brief purpose comments
- Implementation: Comments for non-obvious logic, workarounds, or complex sections
- TODOs: Used sparingly for known issues or future improvements (e.g., `// FIXME: ...`, `// TODO: ...`)

**Style:**
- Block comments: `/* ... */` format, often multi-line for license headers
- Inline comments: `//` format for single-line explanations
- No JSDoc/Doxygen style observed; comments are ad-hoc

**Example** (from `config.h`):
```cpp
/* This is the assumed maximum nesting of configs. Increase if longer paths etc. are required. */
static constexpr size_t MAX_NESTING = 8;
```

## Function Design

**Size:** No explicit size limits observed; functions vary from single-line helpers to large state machines

**Parameters:**
- Use references (`&`) for config objects and mutable parameters
- Use const references (`const &`) for read-only complex objects
- Simple types (int, bool, enum) passed by value
- Lambda captures: Use by-value for thread safety when needed (e.g., `[user, digest_hash]`)

**Return Values:**
- Single return type; no tuple unpacking observed except in generated/utility code
- Void for side-effect only operations (with logging for errors)
- Boolean for success/failure with simple conditions
- String for validation results (empty = success)
- Pointers for optional heap allocations

**Example** (from `batteries.h`):
```cpp
void register_battery_generator(BatteryClassID battery_class, IBatteryGenerator *generator);
IBattery *get_battery(uint32_t slot);
uint32_t get_batteries(BatteryClassID battery_class, IBattery **found_batteries,
                       uint32_t found_batteries_capacity);
bool get_enabled();
```

## Module Design

**Module Base Class:**
- All modules inherit from `IModule` defined in `module.h`
- Lifecycle methods: `pre_init()` → `pre_setup()` → `setup()` → `register_urls()` → `register_events()` → `loop()` → `pre_reboot()`
- Non-copyable: copy constructor and assignment deleted

**Module Pattern** (from `batteries.h`):
```cpp
class Batteries final : public IModule {
public:
    void pre_setup() override;
    void setup() override;
    void register_urls() override;
    void register_events() override;
    void pre_reboot() override;

    // Public interface methods...
private:
    // Implementation details...
};
```

**Exports/Public API:**
- Module registration via global instances or dependency injection
- Configuration state stored in `ConfigRoot` objects
- API endpoints registered via module's `register_urls()` method
- Event subscriptions via `register_events()` method

**Generated Files:**
- Barrel files pattern: `generated/module_available.h` includes conditional compilation directives
- Auto-generated dependencies: `generated/module_dependencies.h`
- Enums from configuration: `generated/battery_class_id.enum.h`

---

*Convention analysis: 2026-03-12*
