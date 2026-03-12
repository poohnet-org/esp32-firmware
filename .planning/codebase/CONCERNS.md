# Codebase Concerns

**Analysis Date:** 2026-03-12

## Tech Debt

**Large Complex Modules:**
- Issue: Several modules exceed safe code complexity and maintenance thresholds
- Files:
  - `software/src/modules/charge_manager/current_allocator.cpp` (2,551 lines) - Core allocation logic with 11+ TODO comments
  - `software/src/modules/meters_modbus_tcp/meter_modbus_tcp.cpp` (3,288 lines) - Modbus protocol implementation
  - `software/src/modules/remote_access/remote_access.cpp` (2,318 lines) - Remote connection handling
- Impact: Difficult to test, refactor, or onboard developers; increased bug surface area; harder to review changes
- Fix approach: Split modules into logical sub-components. Create focused classes around specific responsibilities (e.g., allocator states, Modbus protocol handling). Extract utility functions.

**Incomplete Validation and Error Handling:**
- Issue: Multiple validation TODO comments indicating unfinished bounds checking and input validation
- Files:
  - `software/src/config/conf_int52.cpp` - "TODO check limits"
  - `software/src/config/conf_uint53.cpp` - "TODO check val against limits here"
  - `software/src/modules/batteries_modbus_tcp/batteries_modbus_tcp.cpp` - "FIXME: validate func is valid and vals length match func restrictions"
  - `software/src/modules/charge_manager/current_allocator.cpp` - "TODO: bounds check"
- Impact: Potential invalid configuration acceptance, protocol violations, type confusion in JSON parsing
- Fix approach: Complete all bounded value checks. Add comprehensive validation layer for custom configurations. Add automated tests for boundary values.

**Build System Workarounds:**
- Issue: SCons script limitations documented but unresolved
- Files: `software/pio_hooks.py` (lines 2132, 2221) - "FIXME: Scons runs this script using exec(), resulting in __file__ being not available"
- Impact: Fragile build process, difficult to refactor build scripts, potential relative path issues
- Fix approach: Evaluate alternatives to exec()-based execution. Consider refactoring build hooks into separate module invocations.

**Version Parsing Not Finalized:**
- Issue: Beta version validation skipped
- Files: `software/pio_hooks.py` (line 85) - "FIXME: validate optional beta part"
- Impact: Invalid beta version strings may be accepted; version constraints may not work correctly for beta releases
- Fix approach: Implement full semantic version validation including beta and pre-release suffixes following semver spec.

## Known Bugs

**Charge Manager Config Broken After Update:**
- Symptoms: Charge manager state becomes inconsistent when configuration changes without reboot
- Files: `software/src/modules/charge_manager/charge_manager.cpp` (line 750)
- Trigger: Modify charge manager configuration via web API, then perform operations without rebooting
- Impact: Charger allocation may fail or become inconsistent; user can't change settings without power cycle
- Workaround: Reboot the device after modifying charge manager configuration
- Fix approach: Implement proper state reconstruction on config changes. Add state validation after config updates. Consider holding a reboot requirement for this module in the update flow.

**IPADDR_NONE Include Workaround:**
- Symptoms: Compilation fails with undefined IPADDR_NONE if include order is not maintained
- Files: `software/src/async_https_client.h` (line 23) - "FIXME: without this include here there is a problem with the IPADDR_NONE define in <lwip/ip4_addr.h>"
- Trigger: Include `async_https_client.h` without first including `FS.h`
- Impact: Breaking change if include order is altered; fragile codebase dependency chain
- Fix approach: Move include guards or add explicit include of `lwip/ip4_addr.h` before problematic declarations. Add comment explaining order requirement.

**EEBUS Connection Crash Risk:**
- Symptoms: Potential crash when EEBUS connection is interrupted
- Files: `software/src/modules/eebus/spine_connection.cpp` - "If the connection gets interrupted and removed, this might cause a crash"
- Trigger: Network disconnection during EEBUS communication
- Impact: System instability when connection drops; affects reliability of EV charging coordination
- Fix approach: Add null check guards. Implement connection state guards before using connection pointer. Add integration tests for network failures.

## Memory Management Issues

**Potential Memory Leak in Battery Module:**
- Issue: Battery instances allocated but never destroyed; resources may leak on device lifetime
- Files: `software/src/modules/batteries_modbus_tcp/battery_modbus_tcp.h` (line 102)
- Current pattern: `table` pointer may be allocated in `load_custom_table()` but no corresponding cleanup
- Impact: Long-running systems accumulate memory usage if batteries are added/removed repeatedly
- Fix approach: Implement proper destructor/cleanup pattern. Track allocated tables in managed container. Add teardown on module unload.

**Unsafe String Operations:**
- Issue: Use of `strcpy()`, `sprintf()`, and `vsprintf()` without explicit bounds checking
- Files:
  - `software/src/modules/iso15118/iso15118.cpp` - `strcpy(evseid_iso, "DEWRPE")`
  - `software/src/modules/iso15118/iso20.cpp` - Multiple `strcpy()` calls with fixed strings
  - `software/src/modules/charge_tracker/pdfgen.cpp` - `vsprintf()` calls
  - `software/src/modules/eebus/eebus.cpp` - `sprintf()` calls
- Impact: Buffer overflows if source strings are longer than expected
- Fix approach: Replace with `strncpy()`, bounded `snprintf()`, or use `String` class. Add static assertions on string sizes. Enable compiler warnings for unsafe functions.

## Configuration and Build Issues

**Missing Configuration URLs:**
- Issue: Multiple firmware variants have placeholder/empty configuration URLs
- Files:
  - `software/eltako.ini` (lines 11, 20) - FIXME markers for support email and documentation URLs
  - `software/smart_energy_broker.ini` (lines 11, 14-19) - Multiple FIXME markers for URLs (support, firmware updates, manual, MQTT docs)
- Impact: Users can't access help documentation; support emails not configured
- Fix approach: Fill in actual URLs for each product variant. Validate URL configuration in build system.

## Performance Bottlenecks

**Battery Modbus Burst Writes:**
- Issue: Rapid consecutive writes without delay between operations
- Files: `software/src/modules/batteries_modbus_tcp/battery_modbus_tcp.cpp` (multiple locations marked "FIXME: maybe add a little delay between writes to avoid bursts?")
- Current behavior: Table writer executes sequentially without pacing
- Impact: Network congestion; potential device response timeouts; increased power draw during bulk operations
- Improvement path: Implement configurable delay between write operations. Add exponential backoff on failures. Monitor bandwidth utilization.

**Charge Manager Decision Logic Complexity:**
- Issue: Complex allocation logic with many conditional branches makes runtime performance unpredictable
- Files: `software/src/modules/charge_manager/current_allocator.cpp` - Multiple TODO comments about optimization opportunities
- Current approach: Real-time recalculation without caching or pre-computation
- Impact: High latency during allocation cycles; potential for missed timing windows; CPU spike during decisions
- Improvement path: Profile allocation function. Cache phase-related decisions. Implement batched updates where possible. Consider separate thread for complex calculations.

## Fragile Areas

**Type System Mismatch (Integer Widths):**
- Issue: Multiple uint and int types used for current/power values without consistent semantics
- Files: `software/src/modules/charge_manager/charge_manager.cpp` (line 751) - "TODO: Change all currents everywhere to int32_t or int16_t."
- Current state: Mix of uint16_t, uint32_t, int32_t used interchangeably for current values
- Safe modification: Audit all current-related assignments. Create type-safe wrapper classes. Add bounds checking at boundaries. Add compiler warnings for type conversions.
- Test coverage: Need integration tests for edge cases (0mA, 16A, 32A transitions)

**Deprecated SunSpec Model Without Replacement:**
- Issue: Battery Base Deprecated model (ID 801) still referenced but marked for removal
- Files:
  - `software/src/modules/meters_sun_spec/prepare.py` (line 100)
  - `software/src/modules/meters_sun_spec/generated/sun_spec_model_specs.h` (line 100)
- Impact: Code maintains support for deprecated standard; migration path unclear
- Safe modification: Document which newer model replaces deprecated model. Add deprecation warnings. Create migration script for users. Set EOL date.

**Dynamic Phase Switching Logic:**
- Issue: Complex state machine for phase switching with multiple contingent conditions
- Files: `software/src/modules/phase_switcher/` and related files
- Safe modification: Ensure all state transitions are tested. Add explicit guards for phase count assumptions (1P vs 3P). Verify frequency regulation interaction.
- Test coverage: Need tests for phase transition timing, edge cases during switching

**MQTT Auto-Discovery with Deprecated Schema:**
- Issue: Home Assistant MQTT discovery allows deprecated `object_id` parameter
- Files: `software/src/modules/mqtt_auto_discovery/mqtt_auto_discovery.cpp` (line 228)
- Current behavior: Accepts deprecated but supported field
- Safe modification: Document Home Assistant version requirements. Plan deprecation timeline. Add warnings when deprecated field used. Test with multiple HA versions.

## Configuration System Concerns

**Integer Precision Issues in Config:**
- Issue: ConfInt52 and ConfUint53 types use non-standard JavaScript integer limits (2^53)
- Files:
  - `software/src/config/visitors.h` - Comments indicate uncertainty about necessity
  - `software/src/config/conf_int52.cpp` and `conf_uint53.cpp` - Limits hardcoded as "+-2^53"
- Impact: Mismatch between JSON semantics and C++ integer handling; potential for silent data loss
- Fix approach: Evaluate whether non-standard integer ranges are actually necessary. Consider using string representation for values exceeding safe integer range. Document constraints.

**Configuration Migration Robustness:**
- Issue: Config migration code marked as "TODO: more robust writing"
- Files: `software/src/config_migrations.cpp` and `software/src/modules/certs/certs.cpp`
- Current limitation: File writing not atomic; could corrupt config on power loss during write
- Impact: Config corruption on unexpected power cycles during migrations
- Fix approach: Implement atomic writes (write to temp file, then rename). Add checksums/validation. Test power-loss scenarios.

## Test Coverage Gaps

**Unit Test Limitations:**
- Untested areas: 72 test files exist, but many core modules lack comprehensive test coverage
- Files with no visible test counterpart:
  - `software/src/modules/charge_manager/current_allocator.cpp` (2,551 lines, 11+ TODO comments)
  - `software/src/modules/meters_modbus_tcp/meter_modbus_tcp.cpp` (3,288 lines)
  - `software/src/modules/remote_access/remote_access.cpp` (2,318 lines)
- Risk: Critical business logic changes unvalidated; regressions introduced silently
- Priority: High - These are core modules affecting user experience and safety

**Integration Test Gaps:**
- What's not tested: Full allocation cycles with multiple chargers; phase switching under load; Modbus communication failures
- Files: Python test infrastructure exists but limited C++ test harness
- Risk: Assumptions about module interaction only validated on hardware
- Priority: High - Multi-module interactions critical for safe operation

**Configuration Edge Cases:**
- What's not tested: Malformed configurations; out-of-bounds values; missing required fields; circular dependencies
- Files: Config validation in `software/src/config/visitors.h`
- Risk: Invalid config crashes device or leaves inconsistent state
- Priority: Medium - Config is security boundary and reliability factor

## Dependencies and Versions at Risk

**Arduino ESP32 Package Fragmentation:**
- Risk: Multiple Arduino-ESP32 fork versions present in same codebase
- Impact: Potential compile errors, different behavior across builds
- Files: `software/packages/arduino-esp32#warp-*` - Multiple versions stored simultaneously
- Migration plan: Consolidate to single pinned version. Document reason for fork. Evaluate upstream for feature gaps.

**Generated Code Maintenance:**
- Risk: Large generated files (30KB+ meter specs, 8KB+ Sun Spec models) make diffs unwieldy
- Files: `software/src/modules/meters_modbus_tcp/generated/` and `sun_spec/generated/`
- Impact: Hard to review changes; potential for generator bugs to propagate widely
- Fix approach: Split generated code into smaller files. Add code generation tests. Validate generated output schema.

## Security Considerations

**Include File Ordering Dependency:**
- Risk: Reliance on implicit include order creates silent failures if includes are reordered
- Files: `software/src/async_https_client.h` - FS.h must come before lwip headers
- Recommendation: Add explicit include guards or consolidate related includes

**Unsafe String Operations Security:**
- Risk: `strcpy()` and `sprintf()` can overflow if input validation is incomplete
- Files: `software/src/modules/iso15118/iso20.cpp`, `iso15118.cpp`, `charge_tracker/pdfgen.cpp`
- Recommendation: Migrate to bounded string functions. Enable compiler warnings. Add AFL fuzzing for input parsing.

**Configuration Validation Holes:**
- Risk: Multiple validation TODOs indicate incomplete input validation
- Files: Multiple modules with "FIXME: validate" comments
- Recommendation: Implement defense-in-depth validation. Add schema validation for all user inputs.

---

*Concerns audit: 2026-03-12*
