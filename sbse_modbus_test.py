#!/usr/bin/env python3
"""
SBSE inverter Modbus diagnostic / test tool.

Investigates why "command grid import 2000 W" stopped working after the SBSE
firmware update (3.08.12.R -> 3.16.20.R). Talks to the inverter DIRECTLY,
bypassing the esp32 sbse_controller, so it validates behaviour at the register
level without rebuilding firmware.

Register facts (from the esp32 sbse_controller module + SMA parameter list):
  - Battery power setpoint is a WINDOW, not a single value:
        41467  WSptMax  (Bat.WCtlCom.WSptMax)  -- Maximalsollwert
        41469  WSptMin  (Bat.WCtlCom.WSptMin)  -- Minimalsollwert
        sign convention: positive = DISCHARGE, negative = CHARGE, unit W
  - The controller writes WSptMax = <target>, WSptMin = -15000 (fixed).
    Hypothesis: new firmware no longer honours a negative WSptMax as a
    forced-charge command, so import requests collapse to self-consumption.

Modes:
  read       read-only diagnostic dump (safe any time, no writes)
  discharge  discharge-side test: current-code vs pin-both (safe now, battery full)
  charge     charge-side test matrix (run later, when the battery has room)
  park       write WSptMax = WSptMin = 0 to release the battery

IMPORTANT: disable / stop the esp32 sbse_controller before any write mode,
otherwise it overwrites these registers every ~300 ms and the test is meaningless.

Requires: pip install pymodbus
Tested against pymodbus 2.x and 3.x.
"""

import argparse
import inspect
import sys
import time

try:
    from pymodbus.client import ModbusTcpClient          # pymodbus >= 3.0
except ImportError:                                       # pragma: no cover
    from pymodbus.client.sync import ModbusTcpClient      # pymodbus 2.x

# --------------------------------------------------------------------------
# Connection / register map
# --------------------------------------------------------------------------
INVERTER_IP   = "192.168.110.144"
PORT          = 502
UNIT_INVERTER = 3          # INVERTER_UNIT_ID  in sbse_control_loop.cpp
UNIT_GRID     = 2          # GRID_METER_UNIT_ID

# Battery power setpoint window (written together as a 4-register FC16 block)
WSPTMAX_ADDR  = 41467      # Maximalsollwert Speicher (+discharge / -charge)
WSPTMIN_ADDR  = 41469      # Minimalsollwert Speicher
COMPANION     = -15000     # the value the controller currently pins into WSptMin

# Inverter-level active-power limits (NEW default 0 W in 3.16 -- suspect)
INV_WSPTMAX_ADDR = 41431   # Maximale Wirkleistung  (WModCfg.WCtlComCfg.WSptMax)  RW
INV_WSPTMIN_ADDR = 41433   # Minimale Wirkleistung  (WModCfg.WCtlComCfg.WSptMin)  RW
WMAXIN_ADDR      = 41251   # Begrenzung der Leistungsaufnahme in W (import cap)    WO S32 FIX0
WMAXIN_PCT_ADDR  = 41249   # Begrenzung der Wirkleistungsaufnahme in % (import %)  WO S32 FIX2

# NEW in 3.16: which source the inverter treats as authoritative for W setpoint
WSPT_SOURCE_ADDR = 35547   # Quelle der maximalen Wirkleistungsvorgabe (ENUM)

# Fast shut-down (ASO Setpoint, unit 2, WO): the watchdog's emergency actuator
FSTSTOP_ADDR  = 40018      # Setpoint.PlantControl.Inverter.FstStop
FSTSTOP_STOP  = 381        # stop feed-in
FSTSTOP_START = 1467       # resume operation
FSTSTOP_FULL  = 1749       # full stop (AC disconnect) -- not used by the test

# Measurements (read via FC4 in the controller)
AC_TOTW_ADDR      = 30775  # int32, unit 3, inverter total AC power (GridMs.TotW)
GRID_POWER_ADDR   = 31249  # int32, unit 2, positive = export
PV_POWER_ADDR     = 35469  # uint32, "Leistung PV-Erzeugung" (Measurement.PvGen.PvW)
BATTERY_POWER_ADDR = 31585 # 8 regs: [0..1]=charge W, [6..7]=discharge W
BATTERY_SOC_ADDR  = 30845  # uint32 %
BAT_OPSTATUS_ADDR = 30955  # NEW in 3.16: battery operating status (ENUM)
BAT_VOLT_ADDR     = 30851  # uint32 FIX2 V  (battery voltage)

# BMS / converter limits (RO) -- reveal whether charge is being throttled and by what
BAT_CHA_CUR_MAX   = 32251  # uint32 FIX3 A  (max charge current the BMS allows RIGHT NOW)
BAT_DSCH_CUR_MAX  = 32257  # uint32 FIX3 A  (max discharge current allowed)
CONV_CHA_MAX_W    = 40189  # uint32 FIX0 W  (converter charge power limit, default 15000)
CONV_DSCH_MAX_W   = 40191  # uint32 FIX0 W  (converter discharge power limit, default 3780)
CHA_END_VOLT      = 32239  # uint32 FIX2 V  (charge-end voltage the BMS demands)

WSPT_SOURCE_TAGS = {
    302: "-------", 2357: "Wirkleistungsvorgabe", 4554: "Wirkleistungsvorgabe 2",
    4555: "P(f)-Kennlinie", 4556: "P(U)-Kennlinie", 4557: "Einspeisebegrenzung",
    4998: "Primaerregelleistung (FSM)", 5184: "Integrale lokale Frequenz (ILF)",
    5510: "Unsymmetriebegrenzung der Leistung", 16777213: "Information liegt nicht vor",
}
REDUCTION_ADDR = 30219   # Grund der Leistungsreduzierung (why the inverter throttles)
REDUCTION_TAGS = {
    302: "-------", 557: "Uebertemperatur", 884: "nicht aktiv (none)",
    1704: "Abregelung max. Leistung", 1705: "Frequenzabweichung",
    1706: "Abregelung PV-Strom", 3520: "Spannungsabweichung",
    3554: "Blindleistungsprioritaet", 3556: "Hohe DC-Spannung",
    4560: "Externe Vorgabe", 4561: "Externe Vorgabe 2", 16777213: "n/a",
}
BAT_OPSTATUS_TAGS = {
    303: "Aus", 2291: "Batterie Standby", 2292: "Batterie laden",
    2293: "Batterie entladen", 3664: "Notladebetrieb",
    16777213: "Information liegt nicht vor",
}

# Mode/config enums watched by emswatch (from PARAMETER-SBSExx 3.16 taglists)
WMOD_TAGS = {
    303: "Aus", 1077: "Manuell W", 1078: "Manuell %",
    1079: "Externe Wirkleistungsvorgabe", 16777213: "n/a",
}
VARMOD_TAGS = {
    303: "Aus", 1069: "Q(V)-Kennlinie", 1070: "Q man %", 1071: "Q man var",
    1072: "Q, externe Vorgabe", 1073: "Q(P)-Kennlinie",
    1074: "cos phi, manuelle Vorgabe", 1075: "cos phi, externe Vorgabe",
    1076: "cos phi(P)-Kennlinie", 4450: "Q-Begrenzung",
    4562: "cos phi(V)-Kennlinie", 16777213: "n/a",
}
EXTCTL_TAGS  = {1129: "Yes", 1130: "No", 16777213: "n/a"}
EXC_TAGS     = {1041: "uebererregt", 1042: "untererregt", 16777213: "n/a"}
FSTSTOP_TAGS = {381: "Stop", 1467: "Start", 1749: "Full stop", 16777213: "n/a"}

# Plant-control arbitration (all unit 2). spikewatch showed the charge dips are
# strictly periodic (~19 s) and fall back exactly to self-consumption charging
# while every limiter/derating/BMS diagnostic stays flat -> the inverter's own
# plant controller periodically re-asserts control. 44577 is the documented
# switch that tells it an EXTERNAL controller owns the plant.
USE_EXT_CTL_ADDR = 44577   # Parameter.Operation.PlntCtl.UseExtPlntCtl (RW ENUM)
USE_EXT_CTL_YES  = 1129
USE_EXT_CTL_NO   = 1130    # firmware default
HEARTBEAT_ADDR   = 41465   # Setpoint.ExternalPlantControl.PlntCtl.LifSignRx (RW)
                           # SMA 'Lebenszeichen': external controller writes a
                           # changing value; 32503 counts detected timeouts

# Additional 3.16 diagnostics (from PARAMETER-SBSExx-50_03.16.xx.R) that can
# reveal an inverter-internal limiter kicking in during the periodic charge dips.
RUNSTT_ADDR   = 33003      # Measurement.Operation.RunStt (unit 3) -- 2119 = Derating!
RUNSTT_TAGS = {
    295: "MPP", 302: "-------", 443: "Konstantspannung", 1463: "Backup",
    1855: "Inselbetrieb", 2119: "DERATING", 5477: "Q on Demand",
    40334: "Batterieladung ohne Netz", 16777213: "n/a",
}
WCTL_DMD_ADDR    = 31405   # Measurement.Operation.Dmd.WCtl (unit 2): the W limit
                           # the inverter is CURRENTLY enforcing (whatever source)
PVLIM_NOM_ADDR   = 31245   # Measurement.Inverter.CurWCtlNom (unit 2): internal PV
                           # power limitation in % (zero-export limiter position)
EXTCTL_TMO_ADDR  = 32503   # Measurement.PlntCtl.ExPlntCtlTmOutDetCnt (unit 2):
                           # counts comm timeouts to the external plant controller
                           # -- increments = the inverter declared OUR writes stale

# --------------------------------------------------------------------------
# pymodbus version shims
# --------------------------------------------------------------------------
_UNIT_KW = None  # resolved once against the installed pymodbus

def _resolve_unit_kw(client):
    global _UNIT_KW
    sig = inspect.signature(client.read_input_registers)
    for kw in ("slave", "unit", "device_id"):
        if kw in sig.parameters:
            _UNIT_KW = kw
            return
    _UNIT_KW = None  # very old pymodbus took it positionally; unlikely here

def _unit_kwargs(unit):
    return {_UNIT_KW: unit} if _UNIT_KW else {}


def ensure_connected(client):
    """Reconnect if the socket dropped (the SBSE closes idle/second sessions)."""
    try:
        if getattr(client, "connected", False):
            return True
    except Exception:                               # noqa: BLE001
        pass
    try:
        client.close()
    except Exception:                               # noqa: BLE001
        pass
    time.sleep(0.5)
    try:
        return client.connect()
    except Exception:                               # noqa: BLE001
        return False


def read_regs(client, addr, count, unit):
    """Read `count` registers at `addr`. Tries FC4 (input) then FC3 (holding),
    with one reconnect+retry if the connection dropped."""
    for attempt in range(2):
        last = None
        for fn_name in ("read_input_registers", "read_holding_registers"):
            fn = getattr(client, fn_name)
            try:
                rr = fn(addr, count=count, **_unit_kwargs(unit))
            except Exception as e:                  # noqa: BLE001
                last = e
                continue
            if rr is not None and not rr.isError():
                return rr.registers
            last = rr
        if attempt == 0 and ensure_connected(client):
            continue
        raise RuntimeError(f"read addr={addr} count={count} unit={unit} failed: {last}")


def write_regs(client, addr, values, unit):
    """Write registers, with one reconnect+retry if the connection dropped."""
    for attempt in range(2):
        try:
            rr = client.write_registers(addr, values, **_unit_kwargs(unit))
        except Exception as e:                      # noqa: BLE001
            if attempt == 0 and ensure_connected(client):
                continue
            raise RuntimeError(f"write addr={addr} values={values} unit={unit} failed: {e}")
        if rr is None or rr.isError():
            if attempt == 0 and ensure_connected(client):
                continue
            raise RuntimeError(f"write addr={addr} values={values} unit={unit} failed: {rr}")
        return


# --------------------------------------------------------------------------
# int32 (big-endian, high register first) encode / decode
# --------------------------------------------------------------------------
def s32(regs):
    u = ((regs[0] & 0xFFFF) << 16) | (regs[1] & 0xFFFF)
    return u - (1 << 32) if u >= (1 << 31) else u

def u32(regs):
    return ((regs[0] & 0xFFFF) << 16) | (regs[1] & 0xFFFF)

def to_s32_regs(value):
    u = value & 0xFFFFFFFF
    return [(u >> 16) & 0xFFFF, u & 0xFFFF]

def read_s32(client, addr, unit=UNIT_INVERTER):
    return s32(read_regs(client, addr, 2, unit))

def write_s32(client, addr, value, unit=UNIT_INVERTER):
    write_regs(client, addr, to_s32_regs(value), unit)


# --------------------------------------------------------------------------
# Reads
# --------------------------------------------------------------------------
def read_battery_w(client):
    """+ = discharging, - = charging (matches controller's battery_w_raw)."""
    regs = read_regs(client, BATTERY_POWER_ADDR, 8, UNIT_INVERTER)
    charge_w    = u32(regs[0:2])
    discharge_w = u32(regs[6:8])
    return discharge_w - charge_w, charge_w, discharge_w

def read_soc(client):
    return u32(read_regs(client, BATTERY_SOC_ADDR, 2, UNIT_INVERTER))

def read_scaled(client, addr, div, unit=UNIT_INVERTER):
    return u32(read_regs(client, addr, 2, unit)) / div

def _n(x, nd=3, u=""):
    if x is None:
        return "ERR"
    return f"{x:.{nd}f}{u}" if isinstance(x, float) else f"{x}{u}"

def read_limits(client):
    def rd(addr, div):
        v, _ = try_read(read_scaled, client, addr, div)
        return v
    return {
        "soc":             rd(BATTERY_SOC_ADDR, 1),
        "bat_volt":        rd(BAT_VOLT_ADDR, 100.0),
        "cha_cur_max_A":   rd(BAT_CHA_CUR_MAX, 1000.0),
        "dsch_cur_max_A":  rd(BAT_DSCH_CUR_MAX, 1000.0),
        "conv_cha_max_W":  rd(CONV_CHA_MAX_W, 1),
        "conv_dsch_max_W": rd(CONV_DSCH_MAX_W, 1),
        "cha_end_V":       rd(CHA_END_VOLT, 100.0),
    }

def print_limits(client, label="limits"):
    """Show what the BMS/converter currently allow -- the charge ceiling in particular."""
    d = read_limits(client)
    cur, volt = d["cha_cur_max_A"], d["bat_volt"]
    ceil = f"~{cur * volt:.0f}W" if (cur is not None and volt is not None) else "ERR"
    print(f"  {label}: SoC={_n(d['soc'], 0, '%')}  Vbat={_n(d['bat_volt'], 1, 'V')}  "
          f"maxChargeCurrent={_n(d['cha_cur_max_A'], 3, 'A')}  =>  charge ceiling {ceil}")
    print(f"  {' ' * len(label)}  maxDischargeCurrent={_n(d['dsch_cur_max_A'], 3, 'A')}   "
          f"converter cha/dsch max = {_n(d['conv_cha_max_W'], 0, 'W')}/"
          f"{_n(d['conv_dsch_max_W'], 0, 'W')}   chargeEndV={_n(d['cha_end_V'], 1, 'V')}")

def try_read(fn, *a, **k):
    try:
        return fn(*a, **k), None
    except Exception as e:                          # noqa: BLE001
        return None, e


def snapshot(client, label=""):
    """One-line live summary: grid, battery, SoC, current setpoint window."""
    parts = []

    bat, err = try_read(read_battery_w, client)
    if err is None:
        w, c, d = bat
        parts.append(f"battery={w:+6d}W (chg={c} dis={d})")
    else:
        parts.append(f"battery=ERR({err})")

    soc, err = try_read(read_soc, client)
    parts.append(f"SoC={soc}%" if err is None else f"SoC=ERR({err})")

    gr, err = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
    if err is None:
        g = s32(gr)  # + = export/feed-in
        parts.append(f"grid={g:+6d}W ({'import' if g < 0 else 'export'} {abs(g)}W)")
    else:
        parts.append(f"grid=ERR({err})")

    sp, err = try_read(read_regs, client, WSPTMAX_ADDR, 4, UNIT_INVERTER)
    if err is None:
        parts.append(f"[WSptMax={s32(sp[0:2]):+d} WSptMin={s32(sp[2:4]):+d}]")
    else:
        parts.append(f"setpoint=ERR({err})")

    prefix = f"{label:<22}" if label else ""
    print(f"  {prefix}" + "  ".join(parts))


def cmd_read(client):
    print("\n=== READ-ONLY DIAGNOSTIC DUMP ===\n")

    print("Live state:")
    snapshot(client, "now")

    print("\nBMS / converter limits (the charge-cap suspects):")
    print_limits(client, "now")

    print("\nBattery power setpoint window (what the controller writes):")
    for addr, name in ((WSPTMAX_ADDR, "41467 WSptMax (Maximalsollwert)"),
                       (WSPTMIN_ADDR, "41469 WSptMin (Minimalsollwert)")):
        regs, err = try_read(read_regs, client, addr, 2, UNIT_INVERTER)
        print(f"  {name:<34} = {s32(regs):+d} W" if err is None
              else f"  {name:<34} = ERR({err})")

    print("\nInverter-level active-power limits (NEW: default 0 W in 3.16 -- suspect):")
    for addr, name in ((INV_WSPTMAX_ADDR, "41431 Maximale Wirkleistung"),
                       (INV_WSPTMIN_ADDR, "41433 Minimale Wirkleistung")):
        regs, err = try_read(read_regs, client, addr, 2, UNIT_INVERTER)
        print(f"  {name:<34} = {s32(regs):+d} W" if err is None
              else f"  {name:<34} = ERR({err})")

    print("\nControl-source / status enums:")
    regs, err = try_read(read_regs, client, WSPT_SOURCE_ADDR, 2, UNIT_INVERTER)
    if err is None:
        v = u32(regs)
        print(f"  35547 Quelle max. Wirkleistungsvorgabe = {v} "
              f"({WSPT_SOURCE_TAGS.get(v, '??? unknown tag')})")
    else:
        print(f"  35547 Quelle max. Wirkleistungsvorgabe = ERR({err})")

    regs, err = try_read(read_regs, client, BAT_OPSTATUS_ADDR, 2, UNIT_INVERTER)
    if err is None:
        v = u32(regs)
        print(f"  30955 Batterie-Betriebsstatus          = {v} "
              f"({BAT_OPSTATUS_TAGS.get(v, '??? unknown tag')})")
    else:
        print(f"  30955 Batterie-Betriebsstatus          = ERR({err})")

    print("\nPlant-control arbitration / PCC limiter (unit 2):")
    for addr, name, div, unit in ((USE_EXT_CTL_ADDR, "44577 UseExtPlntCtl (1129=Yes 1130=No)", 1, "u2"),
                                  (WCTL_DMD_ADDR,    "31405 Dmd.WCtl (enforced W limit)",      1, "u2"),
                                  (PVLIM_NOM_ADDR,   "31245 CurWCtlNom (internal PV limit %)", 100.0, "u2"),
                                  (EXTCTL_TMO_ADDR,  "32503 ext-ctl timeout count",            1, "u2")):
        regs, err = try_read(read_regs, client, addr, 2, UNIT_GRID)
        if err is None:
            v = u32(regs) / div if div != 1 else u32(regs)
            print(f"  {name:<42} = {v}")
        else:
            print(f"  {name:<42} = ERR({err})")

    print("\nInterpretation hints:")
    print("  * 35547 should read 'Wirkleistungsvorgabe' (2357) if the Modbus")
    print("    setpoint is the active source. Anything else => the inverter is")
    print("    ignoring your W setpoint, which alone would explain the symptom.")
    print("  * 41431 (inverter Maximale Wirkleistung) reading 0 W could cap output.")
    print("  * export above the feed-in allowance while 31245 reads 100% => the")
    print("    internal PCC feed-in limiter is NOT regulating (see `release` mode).")
    print()


# --------------------------------------------------------------------------
# Write tests
# --------------------------------------------------------------------------
def write_window(client, wmax, wmin):
    """Write the 4-register block exactly like the controller does."""
    write_regs(client, WSPTMAX_ADDR, to_s32_regs(wmax) + to_s32_regs(wmin), UNIT_INVERTER)

def step(client, title, wmax, wmin, settle):
    print(f"\n--- {title}")
    print(f"    write WSptMax={wmax:+d}  WSptMin={wmin:+d}")
    write_window(client, wmax, wmin)
    for i in range(settle):
        time.sleep(1)
        if i == settle - 1:
            snapshot(client, "-> settled")


def confirm(auto_yes):
    print("\n!!  Make sure the esp32 sbse_controller is DISABLED/STOPPED.")
    print("!!  If it is running it overwrites these registers every ~300 ms")
    print("!!  and will also fight for the battery. This test needs exclusive control.")
    if auto_yes:
        print("    (--yes given, proceeding)")
        return True
    ans = input("    Type 'yes' to proceed: ").strip().lower()
    return ans == "yes"


def cmd_discharge(client, mag, settle, auto_yes):
    print("\n=== DISCHARGE TEST (safe with a full battery) ===")
    print("Validates that the proposed 'pin both' fix does not break the working")
    print("discharge path. Expect the battery to DISCHARGE ~%d W in both steps." % mag)
    print("Note: a pinned discharge forces the full %d W even if house load is" % mag)
    print("lower, so the surplus briefly EXPORTS to grid -- that is expected.")
    if not confirm(auto_yes):
        print("Aborted."); return

    snapshot(client, "baseline")
    step(client, f"A) current-code emulation: WSptMax=+{mag}, WSptMin={COMPANION}",
         +mag, COMPANION, settle)
    step(client, f"B) proposed fix (pin both): WSptMax=+{mag}, WSptMin=+{mag}",
         +mag, +mag, settle)
    step(client, "PARK: WSptMax=0, WSptMin=0 (release battery)", 0, 0, settle)

    print("\nExpected: A and B both discharge ~%d W. If so, the fix is safe on" % mag)
    print("the discharge side. (The charge side is the one that reproduces the bug.)")


def cmd_charge(client, mag, settle, auto_yes):
    print("\n=== CHARGE TEST MATRIX (run when the battery has room) ===")
    soc, err = try_read(read_soc, client)
    if err is None and soc >= 98:
        print(f"\n!!  SoC = {soc}% -- battery is essentially full.")
        print("!!  Charging cannot be observed; run this later after some discharge.")
        if not auto_yes:
            if input("    Continue anyway? Type 'yes': ").strip().lower() != "yes":
                print("Aborted."); return
    if not confirm(auto_yes):
        print("Aborted."); return

    snapshot(client, "baseline")
    print_limits(client, "baseline")
    step(client, f"A) reproduce bug: WSptMax=-{mag}, WSptMin={COMPANION}",
         -mag, COMPANION, settle)
    print_limits(client, "during A")
    step(client, f"B) proposed fix (pin both): WSptMax=-{mag}, WSptMin=-{mag}",
         -mag, -mag, settle)
    print_limits(client, "during B")
    step(client, "PARK: WSptMax=0, WSptMin=0 (release battery)", 0, 0, settle)

    print("\nHow to read this:")
    print(f"  * If 'maxChargeCurrent' * Vbat is ~= the watts the battery actually took")
    print(f"    in A/B, the BMS is TAPERING charge at this SoC ({soc if err is None else '?'}%).")
    print(f"    Then pin-both IS the fix -- just re-test at a lower SoC (~40-60%).")
    print(f"  * If maxChargeCurrent is high (many amps) but charge still capped low,")
    print(f"    the limit is in the control path, not the battery -- tell Claude.")


def cmd_chargefix(client, mag, settle, auto_yes):
    """Open the grid-import path and see whether forced charging returns.

    The battery window is pinned to charge -mag throughout. We then open, in
    stages, the registers that could impose a 'no grid import' floor and watch
    whether the battery actually starts pulling ~mag W from the grid."""
    # Keep values within the inverter's rating (a 5 kW unit rejects >~5 kW with
    # IllegalDataValue). We only need to permit slightly more import than we pin.
    OPEN = mag + 2000
    print("\n=== CHARGE-UNBLOCK EXPERIMENT ===")
    print("BMS (~8 kW) and converter (15 kW) both allow charge, yet forced charge caps ~0.")
    print("Live read showed 41433 (inverter 'Minimale Wirkleistung') = 0 W, which forbids")
    print("net grid IMPORT. This opens the import path in stages and checks if charge flows.")
    print(f"Battery window stays pinned to charge -{mag} W the whole time.")
    print(f"Opening import allowance to -{OPEN} W (within the 5 kW rating).")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_max, e1 = try_read(read_s32, client, INV_WSPTMAX_ADDR)
    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    print(f"\nSaved inverter window (restored at end): 41431={inv_max}  41433={inv_min}")

    def hold(label):
        for _ in range(settle):
            time.sleep(1)
        snapshot(client, label)

    def gate(addr, val, name):
        """Best-effort register write; never aborts the experiment."""
        try:
            write_s32(client, addr, val)
            print(f"    wrote {name} = {val:+d}")
            return True
        except Exception as ex:                     # noqa: BLE001
            print(f"    write {name} = {val:+d} REJECTED: {ex}")
            return False

    try:
        snapshot(client, "baseline")
        print_limits(client, "baseline")

        print(f"\n--- 1) pin battery charge -{mag}, gates untouched (expect ~blocked)")
        write_window(client, -mag, -mag)
        hold("-> pinned only")

        # Only the MIN needs to go negative to permit import; leave 41431 (max) alone.
        print(f"\n--- 2) open inverter AC minimum: 41433 = -{OPEN}  (41431 left as-is)")
        gate(INV_WSPTMIN_ADDR, -OPEN, "41433 inv WSptMin")
        write_window(client, -mag, -mag)
        hold("-> inv min open")

        print(f"\n--- 3) also open import caps: 41251(WMaxIn)=+{OPEN}, 41249=100%")
        gate(WMAXIN_ADDR, OPEN, "41251 WMaxIn")
        gate(WMAXIN_PCT_ADDR, 10000, "41249 WMaxIn%")  # FIX2 => 100.00%
        write_window(client, -mag, -mag)
        hold("-> import caps open")
        print_limits(client, "final")

        print("\nInterpretation:")
        print(f"  * charge (battery negative, grid import ~{mag} W) began at step 2")
        print("      -> the inverter WSptMin=0 floor (41433) was the block.")
        print("         Fix: hold 41433 <= -(needed import) while charging.")
        print("  * charge only began at step 3")
        print("      -> the WMaxIn / WMaxIn% import caps (41251/41249, new 0 default) were it.")
        print("  * still ~0 at step 3  -> not these registers; capture this output for Claude.")
    finally:
        print("\nRestoring inverter minimum + import caps and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        # Return the WO import caps to their firmware default (0 = closed) as found.
        for addr, nm in ((WMAXIN_ADDR, "41251"), (WMAXIN_PCT_ADDR, "41249")):
            try: write_s32(client, addr, 0)
            except Exception: pass                  # noqa: BLE001
        try:
            write_window(client, 0, 0)
        except Exception as ex:                     # noqa: BLE001
            print(f"  park failed: {ex}")
        snapshot(client, "restored+parked")


def cmd_chargehold(client, mag, settle, auto_yes):
    """Set 41433 negative + pin the charge ONCE, then observe WITHOUT re-writing.

    Answers the one open question: is 41433 (and the battery window) persistent, or
    does it revert on a watchdog? If charge decays and 41433 reads back 0 after a few
    seconds, the fix must refresh these registers periodically (bypassing the
    deadband while charging)."""
    OPEN = mag + 2000
    dur = max(48, settle)
    print("\n=== CHARGE-SUSTAIN TEST (volatility of 41433 / battery window) ===")
    print(f"Set 41433=-{OPEN} and pin battery -{mag} ONCE, then poll {dur}s without re-writing.")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    try:
        print(f"\nWriting 41433=-{OPEN} and battery window -{mag}/-{mag} once ...")
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        write_window(client, -mag, -mag)

        print("\n  t[s]  battery      41433(invMin)   batWindow[max/min]   grid")
        t = 0
        while t <= dur:
            time.sleep(3); t += 3
            bat, _  = try_read(read_battery_w, client)
            imn, _  = try_read(read_s32, client, INV_WSPTMIN_ADDR)
            win, _  = try_read(read_regs, client, WSPTMAX_ADDR, 4, UNIT_INVERTER)
            gr,  _  = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            bw = f"[{s32(win[0:2]):+d}/{s32(win[2:4]):+d}]" if win else "ERR"
            print(f"  {t:3d}   "
                  f"{(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}     "
                  f"{(f'{imn:+d}' if imn is not None else 'ERR'):>8}        "
                  f"{bw:>16}   "
                  f"{(f'{s32(gr):+d}W' if gr else 'ERR')}")
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def read_window(client):
    win = read_regs(client, WSPTMAX_ADDR, 4, UNIT_INVERTER)
    return s32(win[0:2]), s32(win[2:4])

def cmd_chargewatchdog(client, mag, settle, auto_yes):
    """Find what causes the sporadic charge dips.

    41433 stays open when refreshed, so the floor is not the cause. The next
    suspect is the battery window (41467/41469): it may have an APPLICATION
    watchdog (register keeps its value but the inverter stops driving to it
    unless re-written). Two phases, both keeping 41433 refreshed every 3 s:
      1) FLOOR ONLY   -- battery window written once at the start.
      2) FLOOR+WINDOW -- battery window ALSO re-written every 3 s.
    If phase 1 dips and phase 2 is solid, the window needs periodic refresh
    (the firmware fix: refresh 41467/41469 during charge, bypassing the
    deadband). If both dip, the cause is inverter-internal (e.g. the unbalance
    limiter, 35547=5510) and not these registers. Battery window is read back
    each poll so we can see whether its VALUE holds during a dip."""
    OPEN = mag + 2000
    PERIOD = 3
    DUR = 42
    print("\n=== CHARGE-DIP TEST (does refreshing the battery window help?) ===")
    print(f"Pin battery -{mag}, keep 41433 refreshed; {DUR}s per phase.")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}")
        print("The inverter isn't responding on Modbus. The SBSE usually allows only")
        print("ONE client -- make sure the esp32 controller is fully STOPPED (it auto-")
        print("reconnects if only paused), wait ~10 s, then retry. No writes attempted.")
        return
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    def run_phase(name, refresh_window):
        print(f"\n--- phase: {name}")
        write_window(client, -mag, -mag)
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        print("  t[s]  battery    41433   window[max/min]   grid")
        t = 0
        while t <= DUR:
            time.sleep(PERIOD); t += PERIOD
            try: write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
            except Exception as ex: print(f"    (41433 write failed: {ex})")  # noqa: BLE001
            if refresh_window:
                try: write_window(client, -mag, -mag)
                except Exception as ex: print(f"    (window write failed: {ex})")  # noqa: BLE001
            bat, _ = try_read(read_battery_w, client)
            imn, _ = try_read(read_s32, client, INV_WSPTMIN_ADDR)
            win, _ = try_read(read_window, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            wtxt = f"[{win[0]:+d}/{win[1]:+d}]" if win else "ERR"
            print(f"  {t:3d}   {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}   "
                  f"{(f'{imn:+d}' if imn is not None else 'ERR'):>6}   {wtxt:>15}   "
                  f"{(f'{s32(gr):+d}W' if gr else 'ERR')}")

    try:
        run_phase("FLOOR ONLY (window written once)", refresh_window=False)
        run_phase("FLOOR + WINDOW refreshed every 3s", refresh_window=True)
        print("\nInterpretation:")
        print("  * phase 1 dips, phase 2 solid -> the battery window needs periodic")
        print("    refresh during charge (application watchdog). That's the fix.")
        print("  * both dip -> inverter-internal (unbalance limiter / BMS); check 35547.")
        print("  * note whether window[max/min] still reads -%d/-%d during a dip." % (mag, mag))
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_chargediag(client, mag, settle, auto_yes):
    """Find WHY the charge dips: pin a charge, keep 41433 open, and poll the
    inverter's own reduction reason (30219) + max-power source (35547) + battery
    operating status (30955) fast enough to catch a dip in the act."""
    OPEN = mag + 2000
    PERIOD = 1.5
    DUR = 60
    print("\n=== CHARGE-DIAGNOSIS (why does the charge dip?) ===")
    print(f"Pin battery -{mag}, keep 41433 open, poll {DUR}s. Watch the row where")
    print("'battery' drops well below -%d -- its reduction-reason names the cause." % mag)
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    def tag(addr, tags):
        regs, err = try_read(read_regs, client, addr, 2, UNIT_INVERTER)
        if err is not None:
            return "ERR"
        v = u32(regs)
        return f"{v}:{tags.get(v, '?')}"

    try:
        write_window(client, -mag, -mag)
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        print("\n  t[s]  battery   grid     reduction-reason(30219)      maxsrc(35547)        bat(30955)")
        t = 0.0
        while t <= DUR:
            time.sleep(PERIOD); t += PERIOD
            try: write_s32(client, INV_WSPTMIN_ADDR, -OPEN)   # keep floor open
            except Exception: pass                            # noqa: BLE001
            bat, _ = try_read(read_battery_w, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            red = tag(REDUCTION_ADDR, REDUCTION_TAGS)
            src = tag(WSPT_SOURCE_ADDR, WSPT_SOURCE_TAGS)
            bst = tag(BAT_OPSTATUS_ADDR, BAT_OPSTATUS_TAGS)
            dip = "  <-- DIP" if (bat and bat[0] > -mag + 200) else ""
            print(f"  {t:4.0f}  {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
                  f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}   {red:<28} {src:<20} {bst}{dip}")
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def read_pv_w(client):
    return u32(read_regs(client, PV_POWER_ADDR, 2, UNIT_INVERTER))


def cmd_chargewindow(client, mag, settle, auto_yes):
    """Verify the candidate fix for the PV-ramp clamp while charging.

    Current firmware PINS WSptMin = WSptMax = -mag during charge. In
    zero-export mode that also CAPS PV at (house load + mag): when a cloud
    clears, PV cannot jump -- it only creeps up as fast as the control loop
    raises the pin. Candidate fix: ASYMMETRIC charge window
        WSptMax = -mag          -> 'charge AT LEAST mag' (grid import fills
                                    any gap; needs the 41433 floor open)
        WSptMin = -(mag+2000)   -> inverter may charge MORE from PV surplus
                                    instantly, so PV is free to ramp.
    UNVERIFIED so far: whether a negative WSptMax ALONE really forces the
    minimum charge (all previous forced-charge evidence used the full pin;
    the old default WSptMin=-15000 with WSptMax<0 did NOT force -- but that
    was with 41433 = 0 blocking import, so it proves nothing).
    Two phases, both with 41433 = -(mag+2000) refreshed every 3 s:
      1) PINNED [-mag / -mag]        (current firmware behaviour, baseline)
      2) ASYM   [-mag / -(mag+2000)] (candidate fix)
    Run in DAYLIGHT with PV available and the battery below ~95 % SoC,
    ideally with passing clouds. Watch the pv column: clamped near
    (load + mag) in phase 1, free to jump in phase 2."""
    OPEN = mag + 2000
    PERIOD = 3
    DUR = 90
    print("\n=== CHARGE-WINDOW TEST (does an asymmetric window fix the PV clamp?) ===")
    print(f"Phase 1 pins [-{mag}/-{mag}], phase 2 writes [-{mag}/-{OPEN}]; "
          f"41433=-{OPEN} refreshed; {DUR}s per phase.")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    def run_phase(name, wmax, wmin):
        print(f"\n--- phase: {name}  (window [{wmax:+d}/{wmin:+d}])")
        write_window(client, wmax, wmin)
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        print("  t[s]  battery      pv     grid    41433   window[max/min]")
        t = 0
        while t <= DUR:
            time.sleep(PERIOD); t += PERIOD
            try: write_s32(client, INV_WSPTMIN_ADDR, -OPEN)   # keep floor open
            except Exception as ex: print(f"    (41433 write failed: {ex})")  # noqa: BLE001
            bat, _ = try_read(read_battery_w, client)
            pv,  _ = try_read(read_pv_w, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            imn, _ = try_read(read_s32, client, INV_WSPTMIN_ADDR)
            win, _ = try_read(read_window, client)
            wtxt = f"[{win[0]:+d}/{win[1]:+d}]" if win else "ERR"
            under = "  <-- UNDER-CHARGE (WSptMax not forcing!)" \
                if (bat and bat[0] > -mag + 200) else ""
            print(f"  {t:3d}   {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
                  f"{(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
                  f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}   "
                  f"{(f'{imn:+d}' if imn is not None else 'ERR'):>6}   {wtxt}")

    try:
        run_phase("PINNED (current firmware)", -mag, -mag)
        run_phase("ASYM (candidate fix)",      -mag, -OPEN)
        print("\nInterpretation:")
        print("  * phase 2 battery stays <= -%d at all times AND charges more / pv" % mag)
        print("    rises higher than phase 1 when the sun allows -> candidate is safe:")
        print("    firmware can open WSptMin to -max_charge_w during charge too.")
        print("  * phase 2 rows flagged UNDER-CHARGE (battery well above -%d while" % mag)
        print("    grid does not import the difference) -> negative WSptMax alone does")
        print("    NOT force charging; the pin must stay for grid-import charging and")
        print("    only surplus-charging can be opened.")
        print("  * compare pv between phases only if sky conditions were similar.")
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_spikewatch(client, mag, dur, auto_yes):
    """Catch the periodic charge dips in the act, with the FULL 3.16 diagnostic set.

    Reproduces what the controller does while charging (WSptMax=-mag,
    WSptMin=41433=-(mag+2000), floor refreshed) and polls at 1 Hz:
      battery/pv/grid power        -- the dip itself
      32251*30851 (BMS I*V)        -- BMS charge ceiling in W (taper/balancing?)
      30219 reduction reason       -- derating cause (temp, DC volt, external, ...)
      33003 operating status       -- 2119 = Derating
      35547 max-W source           -- infeed limit / FCR / UNBALANCE limiter active?
      30955 battery op status      -- standby/emergency transitions
      31405 Dmd.WCtl (unit 2)      -- the W limit currently being ENFORCED
      31245 CurWCtlNom (unit 2)    -- internal PV limitation % (zero-export limiter)
      32503 timeout count (unit 2) -- inverter declaring our external control stale
    A row is flagged DIP when battery charge is >200 W short of the pin; any
    diagnostic that CHANGED versus the previous row is listed, so the register
    that moves in lockstep with the dips names the culprit."""
    OPEN = mag + 2000
    PERIOD = 1.0
    print("\n=== SPIKE WATCH (full diagnostic poll during forced charge) ===")
    print(f"Pin battery -{mag}, 41433=-{OPEN} refreshed, poll every {PERIOD:.0f}s for {dur}s.")
    print("Dips recur every ~20-30s, so >= 90s should catch several.")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    def rd_u32(addr, unit=UNIT_INVERTER):
        v, _ = try_read(lambda: u32(read_regs(client, addr, 2, unit)))
        return v

    def rd_tag(addr, tags, unit=UNIT_INVERTER):
        v = rd_u32(addr, unit)
        return "ERR" if v is None else f"{v}:{tags.get(v, '?')}"

    try:
        write_window(client, -mag, -OPEN)
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        print("\n  t[s]  battery      pv     grid   bmsCeil   drt(30219)  run(33003)  "
              "src(35547)  wctl(31405)  pvlim%  tmo")
        prev = {}
        t = 0.0
        next_floor = 0.0
        while t <= dur:
            time.sleep(PERIOD); t += PERIOD
            if t >= next_floor:                      # refresh floor + window every ~3s
                next_floor = t + 3
                try:
                    write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
                    write_window(client, -mag, -OPEN)
                except Exception as ex:              # noqa: BLE001
                    print(f"    (refresh failed: {ex})")

            bat, _ = try_read(read_battery_w, client)
            pv,  _ = try_read(read_pv_w, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            imax   = rd_u32(BAT_CHA_CUR_MAX)         # FIX3 A
            vbat   = rd_u32(BAT_VOLT_ADDR)           # FIX2 V
            ceil_w = (imax / 1000.0) * (vbat / 100.0) if (imax and vbat) else None

            cur = {
                "drt":   rd_tag(REDUCTION_ADDR, REDUCTION_TAGS),
                "run":   rd_tag(RUNSTT_ADDR, RUNSTT_TAGS),
                "src":   rd_tag(WSPT_SOURCE_ADDR, WSPT_SOURCE_TAGS),
                "bst":   rd_tag(BAT_OPSTATUS_ADDR, BAT_OPSTATUS_TAGS),
                "wctl":  rd_u32(WCTL_DMD_ADDR, UNIT_GRID),
                "pvlim": rd_u32(PVLIM_NOM_ADDR, UNIT_GRID),   # FIX2 %
                "tmo":   rd_u32(EXTCTL_TMO_ADDR, UNIT_GRID),
            }
            changed = [k for k in cur if prev and cur[k] != prev.get(k)]
            dip = bat is not None and bat[0] > -mag + 200
            flags = ""
            if dip:
                flags += "  <-- DIP"
            if changed:
                flags += "  CHANGED: " + ", ".join(f"{k}={cur[k]}" for k in changed)
            prev = cur

            pvlim_txt = f"{cur['pvlim'] / 100.0:.0f}%" if cur["pvlim"] is not None else "ERR"
            print(f"  {t:4.0f}  {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
                  f"{(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
                  f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}  "
                  f"{(f'{ceil_w:5.0f}W' if ceil_w is not None else 'ERR'):>7}   "
                  f"{cur['drt'][:16]:<16} {cur['run'][:11]:<11} {cur['src'][:16]:<16} "
                  f"{(str(cur['wctl']) if cur['wctl'] is not None else 'ERR'):>7}  "
                  f"{pvlim_txt:>5}  "
                  f"{(str(cur['tmo']) if cur['tmo'] is not None else 'ERR'):>4}{flags}")

        print("\nInterpretation:")
        print("  * bmsCeil sagging into the dips     -> BYD BMS current-limit taper /")
        print("    balancing; nothing our controller can do (cosmetic, ride through it).")
        print("  * run=2119 DERATING or drt != none  -> inverter-internal derating; the")
        print("    drt tag names the cause (temperature, DC voltage, external setting).")
        print("  * src flips (e.g. 5510 unbalance,   -> another limiter briefly outranks")
        print("    4557 infeed limit) during dips       the Modbus setpoint.")
        print("  * wctl(31405) drops during dips     -> shows the enforced W limit moving")
        print("    even if the source enum lags.")
        print("  * pvlim% dips                        -> the zero-export PV limiter is")
        print("    re-regulating (expected to move with clouds, not every ~25s).")
        print("  * tmo increments                     -> inverter periodically declares our")
        print("    external control STALE; our refresh cadence is too slow for some path.")
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_extctl(client, mag, dur, auto_yes):
    """Test whether 44577 (UseExtPlntCtl = Yes) stops the periodic charge dips.

    spikewatch result (2026-07-22): dips every ~19 s, battery falls back exactly
    to surplus-only charging (grid import stops, slight export), and NO
    limiter/derating/BMS register moves -> the inverter's internal plant
    controller periodically re-asserts self-consumption control.
    44577 Parameter.Operation.PlntCtl.UseExtPlntCtl defaults to 'No'; this mode
    checks whether declaring the plant externally controlled makes the internal
    controller yield permanently.

    Phase A: 44577 as-is (baseline, expect dips every ~19 s)
    Phase B: 44577 = 1129 (Yes), heartbeat 41465 incremented every 3 s
    44577 is RESTORED to its original value afterwards in all cases."""
    OPEN = mag + 2000
    PERIOD = 1.0
    phase_dur = max(60, dur // 2)
    print("\n=== EXTERNAL-PLANT-CONTROL TEST (does 44577=Yes stop the dips?) ===")
    print("!! BLACKLISTED (2026-07-22): toggling 44577 triggers a ONE-WAY parameter")
    print("!! cascade (WMod/VArMod modes end up OFF, WCtlEvu.MbEna disabled -> the")
    print("!! PCC feed-in limiter dies). Repair needs `repair` mode + the web UI.")
    print("!! See events.csv. Requires typed confirmation; --yes is ignored.")
    auto_yes = False
    print(f"Pin battery -{mag}, 41433=-{OPEN} refreshed; two {phase_dur}s phases.")
    print("NOTE: 44577 is a persistent inverter parameter. It is saved first and")
    print("      restored at the end (also on Ctrl-C / errors).")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return

    def rd_u32(addr, unit=UNIT_GRID):
        v, _ = try_read(lambda: u32(read_regs(client, addr, 2, unit)))
        return v

    extctl_orig = rd_u32(USE_EXT_CTL_ADDR)
    if extctl_orig is None:
        print("\nCannot read 44577 -- aborting before any write.")
        return
    tag = {USE_EXT_CTL_YES: "Yes", USE_EXT_CTL_NO: "No"}.get(extctl_orig, "?")
    print(f"\nSaved 41433 = {inv_min}, 44577 = {extctl_orig} ({tag})  (both restored at end)")
    heartbeat = [0]

    def watch(name, send_heartbeat):
        print(f"\n--- phase: {name}")
        print("  t[s]  battery      pv     grid   44577   tmo(32503)")
        dips = 0
        t = 0.0
        next_refresh = 0.0
        while t <= phase_dur:
            time.sleep(PERIOD); t += PERIOD
            if t >= next_refresh:
                next_refresh = t + 3
                try:
                    write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
                    write_window(client, -mag, -OPEN)
                    if send_heartbeat:
                        heartbeat[0] = (heartbeat[0] + 1) % 65536
                        write_regs(client, HEARTBEAT_ADDR,
                                   to_s32_regs(heartbeat[0]), UNIT_GRID)
                except Exception as ex:              # noqa: BLE001
                    print(f"    (refresh failed: {ex})")
            bat, _ = try_read(read_battery_w, client)
            pv,  _ = try_read(read_pv_w, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            ext    = rd_u32(USE_EXT_CTL_ADDR)
            tmo    = rd_u32(EXTCTL_TMO_ADDR)
            dip = bat is not None and bat[0] > -mag + 200
            if dip:
                dips += 1
            print(f"  {t:4.0f}  {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
                  f"{(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
                  f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}  "
                  f"{(str(ext) if ext is not None else 'ERR'):>6}  "
                  f"{(str(tmo) if tmo is not None else 'ERR'):>6}"
                  f"{'  <-- DIP' if dip else ''}")
        print(f"  => {dips} DIP rows in {phase_dur}s "
              f"(expect ~{phase_dur // 19}x1-3 rows if dips continue every ~19s)")
        return dips

    try:
        dips_a = watch(f"A) baseline (44577={extctl_orig})", send_heartbeat=False)

        print(f"\nWriting 44577 = {USE_EXT_CTL_YES} (Yes, plant externally controlled)")
        write_regs(client, USE_EXT_CTL_ADDR, to_s32_regs(USE_EXT_CTL_YES), UNIT_GRID)
        dips_b = watch("B) 44577=Yes + heartbeat", send_heartbeat=True)

        print("\nInterpretation:")
        print(f"  * A dipped ({dips_a} rows), B clean (0)  -> 44577 is THE fix: the")
        print("    firmware should set it once at connect (and the web UI may expose it).")
        print("    Re-check discharge/park behaviour afterwards before adopting!")
        print("  * both phases dip the same           -> 44577 is not it; next suspects")
        print("    are inverter-internal EMS cycles we cannot configure via Modbus.")
        print("  * B dips but tmo(32503) increments   -> heartbeat cadence/format wrong;")
        print("    the inverter periodically declares us stale. Tell Claude.")
    finally:
        print(f"\nRestoring 44577 = {extctl_orig}, 41433, and parking battery ...")
        try:
            write_regs(client, USE_EXT_CTL_ADDR, to_s32_regs(extctl_orig), UNIT_GRID)
        except Exception as ex:                      # noqa: BLE001
            print(f"  !! restore 44577 failed: {ex}")
            print(f"  !! restore manually: write {extctl_orig} to 44577 (unit 2)")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_extctlhold(client, mag, dur, auto_yes):
    """Answer the two design questions before adopting 44577 in firmware.

    extctl proved: 44577=Yes + heartbeat + setpoint refresh => zero dips.
    Still unknown:
      1) Is the HEARTBEAT (41465) needed at all, or was 44577 alone enough?
      2) FAILSAFE: when the external controller goes silent with 44577=Yes,
         does the inverter fall back to internal control (and how fast)?
    Phase 1: 44577=Yes, floor+window refreshed every 3 s, NO heartbeat.
             Dips return -> heartbeat was the essential part.
             Clean      -> heartbeat unnecessary while setpoints are written.
    Phase 2: 44577 stays Yes but ALL writes stop (simulated dead ESP32).
             Watch how long the pinned charge persists, whether 41433 decays,
             whether tmo(32503) increments, and whether the battery returns to
             self-consumption (internal EMS resuming = the failsafe works).
    44577 is RESTORED to its original value at the end in all cases."""
    OPEN = mag + 2000
    PERIOD = 1.0
    phase_dur = max(60, dur // 2)
    print("\n=== EXT-CTL HOLD TEST (heartbeat needed? failsafe on silence?) ===")
    print("!! BLACKLISTED (2026-07-22): writes 44577 -- see the warning in extctl.")
    print("!! Requires typed confirmation; --yes is ignored.")
    auto_yes = False
    print(f"Pin battery -{mag}; 44577=Yes; phase 1 refresh-no-heartbeat {phase_dur}s,")
    print(f"phase 2 total silence {phase_dur}s (only reads).")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return

    def rd_u32(addr, unit=UNIT_GRID):
        v, _ = try_read(lambda: u32(read_regs(client, addr, 2, unit)))
        return v

    extctl_orig = rd_u32(USE_EXT_CTL_ADDR)
    if extctl_orig is None:
        print("\nCannot read 44577 -- aborting before any write.")
        return
    print(f"\nSaved 41433 = {inv_min}, 44577 = {extctl_orig}  (both restored at end)")

    def poll_row(t):
        bat, _ = try_read(read_battery_w, client)
        pv,  _ = try_read(read_pv_w, client)
        gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
        imn, _ = try_read(read_s32, client, INV_WSPTMIN_ADDR)
        win, _ = try_read(read_window, client)
        tmo    = rd_u32(EXTCTL_TMO_ADDR)
        dip = bat is not None and bat[0] > -mag + 200
        wtxt = f"[{win[0]:+d}/{win[1]:+d}]" if win else "ERR"
        print(f"  {t:4.0f}  {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
              f"{(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
              f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}  "
              f"{(f'{imn:+d}' if imn is not None else 'ERR'):>6}  {wtxt:>15}  "
              f"{(str(tmo) if tmo is not None else 'ERR'):>4}{'  <-- DIP' if dip else ''}")
        return dip

    try:
        print(f"\nWriting 44577 = {USE_EXT_CTL_YES} (Yes)")
        write_regs(client, USE_EXT_CTL_ADDR, to_s32_regs(USE_EXT_CTL_YES), UNIT_GRID)

        print(f"\n--- phase 1: refresh floor+window every 3s, NO heartbeat ({phase_dur}s)")
        print("  t[s]  battery      pv     grid   41433   window[max/min]   tmo")
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        write_window(client, -mag, -OPEN)
        dips1 = 0
        t = 0.0
        next_refresh = 0.0
        while t <= phase_dur:
            time.sleep(PERIOD); t += PERIOD
            if t >= next_refresh:
                next_refresh = t + 3
                try:
                    write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
                    write_window(client, -mag, -OPEN)
                except Exception as ex:              # noqa: BLE001
                    print(f"    (refresh failed: {ex})")
            if poll_row(t):
                dips1 += 1
        print(f"  => {dips1} DIP rows (dips back = heartbeat IS needed; clean = 44577+writes suffice)")

        print(f"\n--- phase 2: TOTAL SILENCE, reads only ({phase_dur}s) -- simulated dead ESP32")
        print("  t[s]  battery      pv     grid   41433   window[max/min]   tmo")
        t = 0.0
        while t <= phase_dur:
            time.sleep(PERIOD); t += PERIOD
            poll_row(t)

        print("\nInterpretation:")
        print("  * phase 1 clean                  -> firmware fix = write 44577=Yes once at")
        print("    connect; no heartbeat needed (setpoint writes are the life sign).")
        print("  * phase 1 dips every ~19s        -> heartbeat 41465 is the life sign;")
        print("    firmware must write it periodically (piggyback on the 5s floor refresh).")
        print("  * phase 2: note the seconds until charge stops / self-consumption resumes")
        print("    and whether 41433 reads 0 again -- that's the inverter failsafe delay.")
        print("  * phase 2 battery stays pinned forever with grid import -> NO failsafe;")
        print("    firmware MUST restore 44577=No whenever the controller is disabled.")
    finally:
        print(f"\nRestoring 44577 = {extctl_orig}, 41433, and parking battery ...")
        try:
            write_regs(client, USE_EXT_CTL_ADDR, to_s32_regs(extctl_orig), UNIT_GRID)
        except Exception as ex:                      # noqa: BLE001
            print(f"  !! restore 44577 failed: {ex}")
            print(f"  !! restore manually: write {extctl_orig} to 44577 (unit 2)")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_release(client, settle, auto_yes):
    """Hand PCC regulation back to the inverter after external-control tests.

    Incident (2026-07-22): after the 44577 experiments ended (44577 restored to
    No, battery parked 0/0), the inverter no longer curtailed PV -- the whole
    surplus (PV + fuel cell - load) exported, far beyond the 750 W allowance.
    Hypothesis: an external-plant-control session latches while external
    setpoints are considered live; the internal PCC feed-in limiter stays
    suspended until the session ends (possibly only via the 600 s timeout,
    41525, since 41193 fallback = 'Values maintained').

    This mode steps through safe hand-back actions, each followed by an
    observation window. STOP at the first step where curtailment resumes
    (grid export drops toward the allowance / 31245 pvlim%% starts moving):
      0) baseline observation
      1) 41433 (inverter WSptMin) = 0        -- its firmware default
      2) 44577 = 1130 (No)                   -- idempotent re-assert
    (A former step 3 toggled 44577 Yes->No; removed -- the event log showed
    every 44577 write triggers a destructive one-way parameter cascade.)
    If these don't bring the limiter back, run `repair` and restore
    WCtlEvu.MbEna via the web UI; inverter reboot as last resort.
    All values written here are the firmware defaults -- nothing to restore."""
    STEP = max(30, settle)
    PERIOD = 3
    print("\n=== RELEASE (give PCC control back to the inverter) ===")
    print(f"Observation window per step: {STEP}s. Battery stays parked 0/0.")
    if not confirm(auto_yes):
        print("Aborted."); return

    def rd_u32g(addr):
        v, _ = try_read(lambda: u32(read_regs(client, addr, 2, UNIT_GRID)))
        return v

    def observe(label):
        print(f"\n--- observe: {label}")
        print("  t[s]      pv     grid   pvlim%   wctl   44577")
        t = 0
        while t <= STEP:
            time.sleep(PERIOD); t += PERIOD
            pv, _ = try_read(read_pv_w, client)
            gr, _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
            lim   = rd_u32g(PVLIM_NOM_ADDR)
            wctl  = rd_u32g(WCTL_DMD_ADDR)
            ext   = rd_u32g(USE_EXT_CTL_ADDR)
            g = s32(gr) if gr else None
            over = "  <-- EXPORT OVER LIMIT?" if (g is not None and g > 800) else ""
            print(f"  {t:3d}  {(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
                  f"{(f'{g:+5d}W' if g is not None else 'ERR'):>7}  "
                  f"{(f'{lim / 100.0:5.0f}%' if lim is not None else 'ERR'):>6}  "
                  f"{(str(wctl) if wctl is not None else 'ERR'):>5}  "
                  f"{(str(ext) if ext is not None else 'ERR'):>6}{over}")

    try:
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  (park failed: {ex})")   # noqa: BLE001
        observe("0) baseline (battery parked, no changes yet)")

        print("\n>>> step 1: 41433 = 0 (inverter WSptMin back to firmware default)")
        try: write_s32(client, INV_WSPTMIN_ADDR, 0)
        except Exception as ex: print(f"    write failed: {ex}")  # noqa: BLE001
        observe("1) after 41433 = 0")

        print("\n>>> step 2: 44577 = 1130 (No) re-asserted")
        try: write_regs(client, USE_EXT_CTL_ADDR, to_s32_regs(USE_EXT_CTL_NO), UNIT_GRID)
        except Exception as ex: print(f"    write failed: {ex}")  # noqa: BLE001
        observe("2) after 44577 = No")

        # Step 3 (44577 Yes->No toggle) REMOVED 2026-07-22: the inverter event log
        # revealed that every 44577 write triggers a one-way parameter cascade
        # (WMod/VArMod -> Off, WCtlEvu.MbEna -> Aus). Toggling it to "kick the
        # state machine" makes the damage worse, not better. Use `repair` + the
        # web UI instead.

        print("\nIf export is still over the allowance with 31245 stuck at 100%:")
        print("  * wait ~10 min (41525 external-setpoint timeout is 600 s) and re-run")
        print("    `read` -- the session may expire on its own; or")
        print("  * reboot the inverter via its web UI (persistent state is all-default")
        print("    now, so it starts with internal PCC control active).")
        print("Note which step (if any) brought curtailment back -- that defines the")
        print("hand-back sequence the ESP32 firmware must use on disable/park.")
    finally:
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")     # noqa: BLE001
        snapshot(client, "parked")


def cmd_timeout(client, set_s, auto_yes):
    """Inspect (and optionally shorten) the external-control timeouts.

    Working theory from the 2026-07-22 incident: external plant-control state
    latches until the external-setpoint timeout expires; 41525 (default 600 s)
    is the best-documented candidate for what eventually ended the session and
    let the internal PCC feed-in limiter resume. Shortening it to ~60 s bounds
    how long stale external state (forced charge, suspended feed-in limiter)
    can outlive a crashed/stopped controller. The running controller refreshes
    its setpoints every <= 5 s, so 60 s is far outside normal operation.

    Without --set this is read-only. With --set <seconds> it writes 41525
    (FIX2, so seconds*100) and reads it back. The value is a PERSISTENT
    inverter parameter -- note the old value if you may want to revert."""
    TIMEOUT_REGS = (
        (41525, "41525 WCtlCom      (ext. active power setpoint)"),
        (41527, "41527 WCtlCom2     (ext. active power setpoint 2)"),
        (41529, "41529 VArCtlCom    (ext. reactive power)"),
        (41531, "41531 PFCtlCom     (ext. cos phi)"),
        (41533, "41533 VArCtlVolCom (Q(V) reference voltage)"),
    )
    FALLBACK_TAGS = {2506: "Values maintained", 2507: "Apply fallback values"}
    FALLBACK_REGS = (
        (41193, "41193 WCtlCom  fallback behavior"),
        (41445, "41445 WCtlCom2 fallback behavior"),
        (41219, "41219 VArCtlCom fallback behavior"),
        (41225, "41225 PFCtlCom fallback behavior"),
    )

    print("\n=== EXTERNAL-CONTROL TIMEOUTS ===\n")
    print("Timeouts (FIX2, seconds):")
    for addr, name in TIMEOUT_REGS:
        regs, err = try_read(read_regs, client, addr, 2, UNIT_INVERTER)
        print(f"  {name:<48} = {u32(regs) / 100.0:8.2f} s" if err is None
              else f"  {name:<48} = ERR({err})")
    print("\nFallback behavior on timeout:")
    for addr, name in FALLBACK_REGS:
        regs, err = try_read(read_regs, client, addr, 2, UNIT_INVERTER)
        if err is None:
            v = u32(regs)
            print(f"  {name:<48} = {v} ({FALLBACK_TAGS.get(v, '?')})")
        else:
            print(f"  {name:<48} = ERR({err})")

    if set_s is None:
        print("\n(read-only; pass --set <seconds> to change 41525)")
        return

    if not (0.01 <= set_s <= 1800.0):
        print(f"\n--set {set_s} out of range (0.01 .. 1800 s); not writing.")
        return
    print(f"\nAbout to write 41525 = {set_s:.2f} s (persistent parameter!)")
    if not confirm(auto_yes):
        print("Aborted."); return
    raw = int(round(set_s * 100.0))
    try:
        write_regs(client, 41525, to_s32_regs(raw), UNIT_INVERTER)
    except Exception as ex:                          # noqa: BLE001
        print(f"  write failed: {ex}")
        return
    regs, err = try_read(read_regs, client, 41525, 2, UNIT_INVERTER)
    print(f"  read-back: 41525 = {u32(regs) / 100.0:.2f} s" if err is None
          else f"  read-back failed: {err}")


def cmd_repair(client, auto_yes):
    """Undo the parameter damage caused by toggling 44577 (UseExtPlntCtl).

    The inverter event log (events.csv, 2026-07-22) shows that writing 44577
    triggers a CASCADE by user 'system':
      Yes: WMod -> 'Externe Wirkleistungsvorgabe', VArModOut/In/ZerW ->
           'Q, externe Vorgabe', WCtlEvu.MbEna Ein -> AUS,
           'Wirkleistungsbegrenzung wird in Anlage versendet'
      No:  all of the above modes -> AUS (NOT restored to their originals!)
    Net damage after our Yes/No test cycles (originals from the log's 'von
    Wert' fields):
      40210 WMod       now Aus, was 1079 'Externe Wirkleistungsvorgabe'
      41319 VArModOut  now Aus, was 1074 'cos phi, manuelle Vorgabe'
      41321 VArModIn   now Aus, was 1074 'cos phi, manuelle Vorgabe'
      41323 VArModZerW was Aus originally -> should still read 303 (check only)
      WCtlEvu.MbEna    now Aus, was Ein -- NOT Modbus-accessible: restore via
                       the inverter web UI (this is the feed-in limiter path!)
    This mode restores the three Modbus-reachable parameters with read-back,
    and verifies the check-only ones. The cos-phi VALUE registers were never
    touched (only the mode), so re-enabling the mode restores the old setpoint."""
    # The event log shows the cascade hit TWO levels: 'Sunny Boy Smart Energy'
    # rows = device level (unit 3), 'Plant:1' rows = the embedded System
    # Manager (unit 2). 41319/41321/41323 exist on BOTH units; 40210 is
    # unit-3-only, 44577 unit-2-only.
    RESTORE = (
        (40210, UNIT_INVERTER, 1079, "40210@dev   WModCfg.WMod        -> 1079 Externe Wirkleistungsvorgabe"),
        (41319, UNIT_INVERTER, 1074, "41319@dev   VArModCfg.VArModOut -> 1074 cos phi, manuelle Vorgabe"),
        (41321, UNIT_INVERTER, 1074, "41321@dev   VArModCfg.VArModIn  -> 1074 cos phi, manuelle Vorgabe"),
        (41319, UNIT_GRID,     1074, "41319@plant VArModCfg.VArModOut -> 1074 cos phi, manuelle Vorgabe"),
        (41321, UNIT_GRID,     1074, "41321@plant VArModCfg.VArModIn  -> 1074 cos phi, manuelle Vorgabe"),
    )
    CHECK = (
        # (addr, unit, expected, label) -- reading a register on a unit that
        # doesn't host it returns 0xFFFFFFFF 'information not available'.
        (41323, UNIT_INVERTER, 303,  "41323@dev   VArModZerW (expect 303 Aus)"),
        (41323, UNIT_GRID,     303,  "41323@plant VArModZerW (expect 303 Aus)"),
        (44577, UNIT_GRID,     1130, "44577@plant UseExtPlntCtl (expect 1130 Nein)"),
    )
    print("\n=== REPAIR (restore parameters clobbered by the 44577 cascade) ===")
    print("Restores WMod + VArModOut/In to their event-log 'from' values.")
    print("NOTE: WCtlEvu.MbEna (Ein) must be restored via the inverter WEB UI --")
    print("      it has no Modbus register. Do that too, then re-test the 750 W cap.")
    if not confirm(auto_yes):
        print("Aborted."); return

    for addr, unit, want, label in RESTORE:
        regs, err = try_read(read_regs, client, addr, 2, unit)
        before = u32(regs) if err is None else None
        if before == want:
            print(f"  {label:<74} already {want}, skipping")
            continue
        try:
            write_regs(client, addr, to_s32_regs(want), unit)
        except Exception as ex:                      # noqa: BLE001
            print(f"  {label:<74} WRITE REJECTED ({ex}) -- set it via the web UI")
            continue
        regs, err = try_read(read_regs, client, addr, 2, unit)
        after = u32(regs) if err is None else None
        ok = "OK" if after == want else f"READ-BACK MISMATCH ({after})"
        print(f"  {label:<74} was {before}, now {after}: {ok}")

    print("\nCheck-only:")
    for addr, unit, want, label in CHECK:
        regs, err = try_read(read_regs, client, addr, 2, unit)
        v = u32(regs) if err is None else None
        print(f"  {label:<50} = {v} {'OK' if v == want else '<-- UNEXPECTED, investigate'}")

    print("\nRemaining manual steps (inverter web UI):")
    print("  1. Re-enable Parameter.Inverter.CtlComCfg.WCtlEvu.MbEna (Ein).")
    print("  2. Verify the feed-in limitation setting (750 W) is still configured.")
    print("  3. With the battery parked and surplus available, confirm export is")
    print("     capped at ~750 W again.")
    print("  4. Re-test grid-import charging (WMod was Off; the 41433 floor was")
    print("     likely ignored meanwhile).")


def cmd_floorrate(client, mag, dur, rates, auto_yes):
    """Verify the dip mechanism and the proposed 1 s floor-refresh fix BEFORE
    changing firmware.

    monitor2.txt (2026-07-26) proved: the System Manager re-sends device
    setpoint DEFAULTS on an exact 20 s beat, resetting 41433 (inverter WSptMin,
    the grid-import floor) to 0. The controller's 5 s refresh restores it; the
    1-5 s gap is the charge dip. Proposed firmware fix: refresh 41433 every
    1 s while a charge floor is active -> dips shrink to <= 1 s.
    This mode emulates candidate refresh rates under a pinned forced charge:
    one phase per value in --rates (seconds, e.g. --rates 5,1,0.3; 0.3 =
    the firmware's 300 ms tick, i.e. 'refresh every tick').
    Measured 2026-07-26 (--mag 2000, 120 s/phase): 5 s -> dips 3-5.5 s,
    total 18 s; 1 s -> dips 0.5-1.5 s, total 4.5 s. Open question: does
    0.3 s beat 1 s, or is the ~0.5 s device reaction time the floor?
    Each phase polls ~2 Hz and logs only interesting rows (floor found reset,
    dip active, or a 15 s heartbeat), then reports floor resets seen and each
    dip episode's duration. The ~20 s reset beat is external -- expect the
    same ~dur/20 dips per phase; only their DURATION should change.
    Run with the esp32 controller fully STOPPED (its own refresh would
    contaminate the timing) and SoC low enough to accept -mag W of charge."""
    OPEN = mag + 2000
    POLL = 0.5
    print("\n=== FLOOR-REFRESH-RATE TEST (dip length vs 41433 refresh rate) ===")
    print(f"Pin battery -{mag} (window [-{mag}/-{OPEN}]), 41433=-{OPEN}; "
          f"{dur}s per phase; rates: {', '.join(f'{r:g}s' for r in rates)}.")
    if not confirm(auto_yes):
        print("Aborted."); return

    inv_min, e2 = try_read(read_s32, client, INV_WSPTMIN_ADDR)
    if e2 is not None:
        print(f"\nCannot read 41433: {e2}. Is the controller fully stopped? Retry.")
        return
    print(f"\nSaved 41433 = {inv_min} (restored at end)")

    def run_phase(name, period):
        print(f"\n--- phase: {name} (41433 refreshed every {period:g} s)")
        print("  t[s]   battery     grid    41433   flags")
        write_window(client, -mag, -OPEN)
        write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
        resets = 0
        episodes = []          # completed dip durations [s]
        dip_start = None
        floor_was_open = True
        poll = min(POLL, period)   # fast rates need a faster loop
        start = time.time()
        t = 0.0
        next_refresh = period
        next_beat = 15.0
        while t <= dur:
            time.sleep(poll)
            t = time.time() - start   # wall clock: transaction time must count
            if t >= next_refresh:
                next_refresh = t + period
                try:
                    write_s32(client, INV_WSPTMIN_ADDR, -OPEN)
                except Exception as ex:              # noqa: BLE001
                    print(f"    (floor write failed: {ex})")
            imn, _ = try_read(read_s32, client, INV_WSPTMIN_ADDR)
            bat, _ = try_read(read_battery_w, client)
            gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)

            floor_open = (imn == -OPEN)
            if floor_was_open and imn is not None and not floor_open:
                resets += 1
            floor_was_open = floor_open if imn is not None else floor_was_open

            dip = bat is not None and bat[0] > -mag + 200
            if dip and dip_start is None:
                dip_start = t
            elif not dip and dip_start is not None:
                episodes.append(t - dip_start)
                dip_start = None

            flags = ""
            if imn is not None and not floor_open:
                flags += f"  <-- FLOOR RESET (#{resets})"
            if dip:
                flags += "  DIP"
            if flags or t >= next_beat:
                next_beat = t + 15.0
                print(f"  {t:5.1f}  {(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
                      f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}  "
                      f"{(f'{imn:+d}' if imn is not None else 'ERR'):>6}{flags}")
        if dip_start is not None:
            episodes.append(t - dip_start)
        total_dip = sum(episodes)
        eps = ", ".join(f"{d:.1f}s" for d in episodes) if episodes else "none"
        print(f"  => {resets} floor resets seen, {len(episodes)} dip episodes "
              f"({eps}), total dip time {total_dip:.1f}s of {dur}s")
        return resets, episodes, total_dip

    try:
        results = []
        for i, rate in enumerate(rates):
            label = f"{chr(65 + i)}) {rate:g} s refresh"
            resets, episodes, total_dip = run_phase(label, rate)
            results.append((rate, resets, episodes, total_dip))
        print(f"\nComparison ({dur}s per phase, ~{dur // 20} external resets expected each):")
        print("  rate       resets  dip episodes  longest dip  total dip time")
        for rate, resets, episodes, total_dip in results:
            longest = f"{max(episodes):.1f}s" if episodes else "-"
            print(f"  {rate:5g} s   {resets:6d}  {len(episodes):12d}  {longest:>11}  "
                  f"{total_dip:10.1f} s")
        print("\nInterpretation:")
        print("  * dip DURATION should track the refresh period until it saturates at")
        print("    the device reaction time (~0.5 s down + ~0.5 s recover). The rate")
        print("    where it stops improving is the right firmware refresh interval.")
        print("  * resets seen can undercount at fast rates (the 41433=0 window gets")
        print("    shorter than the poll) -- count dip episodes instead.")
        print("  * a rate with MORE/LONGER dips than a slower one -> Modbus congestion")
        print("    is hurting; back off.")
    finally:
        print("\nRestoring 41433 and parking battery ...")
        if e2 is None:
            try: write_s32(client, INV_WSPTMIN_ADDR, inv_min)
            except Exception as ex: print(f"  restore 41433 failed: {ex}")  # noqa: BLE001
        try: write_window(client, 0, 0)
        except Exception as ex: print(f"  park failed: {ex}")  # noqa: BLE001
        snapshot(client, "restored+parked")


def cmd_emswatch(client, dur, focus=False):
    """Passively observe which plant-control registers the INTERNAL energy
    management uses while it controls the inverter. READ-ONLY: zero writes,
    no confirmation needed, Ctrl-C safe (no park on exit).

    Two experiment setups (2026-07-24):
      (a) internal EMS ON + esp32 controller STOPPED: discover the EMS's
          actuator set (done -- see monitor.txt: 41467 ~1/s with 41469=-15000,
          41431 ramped at 120 W/s, PV curtailment only via internal 31245).
      (b) internal EMS OFF + esp32 controller RUNNING (use --focus): verify
          our writes land in the same registers and catch anything internal
          overwriting them periodically (the ~19 s dip beat). --focus limits
          the sweep to the window/status registers for ~3 Hz sampling -- a
          full sweep (~1-2 s) would miss a flip that our controller's ~300 ms
          refresh overwrites again. NOTE: setup (b) runs THREE Modbus clients
          (esp32 + Node-RED + this script); many ERRs in the probe = the SBSE
          connection limit, not EMS activity. A dip in battery power with NO
          register flip even at 3 Hz is a decisive result too: arbitration
          then happens BELOW the Modbus-visible register layer.

    Purpose (2026-07-24): before building sbse-controller 2.0, discover the
    internal System Manager's actual actuator set the same way the Node-RED
    40016 flow was once discovered -- watch every plant-control register while
    the internal EMS runs and see which ones move, with what values/cadence.

    Procedure:
      1. web UI: turn the internal energy management back ON
      2. fully STOP the esp32 sbse_controller (not just paused)
      3. leave Node-RED RUNNING -- the 40016 feed-in cap falls back to 0 %
         without its regular refresh, which would curtail PV and distort the
         observation. Its known, constant effect (Dmd.WCtl 31405 ~ 748 W with
         fuel cell on, ~0 W with it off) is easy to discount in the log.
      4. run with --dur 600 (or more) across interesting situations: PV
         surplus, load steps, a forced charge started in the web UI, ...

    Watches (initial probe decides what is readable; WO registers that read
    as errors are dropped, ones that read 'n/a' stay -- flipping from n/a to
    a value IS a discovery):
      * every readable Setpoint.PlantControl.* / Setpoint.ExternalPlantControl.*
        register on both units (battery window, inverter window, W limits,
        import caps, Q/cos-phi, DSO minima, heartbeat, fast stop, ...)
      * the 44577-cascade mode parameters (44577, 40210, 41319/21/23 on both
        units) as tripwires -- these must NOT move; if they do, note the time
      * enforced-limit/status registers (31405 Dmd.WCtl, 31245/31241 PV lim %,
        35547 W-limit source, 30219/33003/30955, 32503/05/07 timeout counters)
    Output: full baseline snapshot, then one block per poll cycle in which
    something CHANGED (with a bat/pv/grid context line), a plain context row
    every 30 s, and a per-register change summary at the end (also on Ctrl-C)."""
    U2, U3 = UNIT_GRID, UNIT_INVERTER
    # (addr, unit, count, kind, div, deadband_in_scaled_units, tags, label)
    CATALOG = (
        # setpoint channels (ASO) -- unit 3 (device)
        (41467, U3, 2, "s32", 1,    25, None,         "Bat WSptMax [W]"),
        (41469, U3, 2, "s32", 1,    25, None,         "Bat WSptMin [W]"),
        (41431, U3, 2, "s32", 1,    25, None,         "Inv WSptMax [W]"),
        (41433, U3, 2, "s32", 1,    25, None,         "Inv WSptMin [W]"),
        (41249, U3, 2, "s32", 100,   1, None,         "WMaxIn [%]"),
        (41251, U3, 2, "s32", 1,    25, None,         "WMaxIn [W]"),
        (44039, U3, 2, "s32", 100,   1, None,         "Inv WSptMax [%]"),
        (44041, U3, 2, "s32", 100,   1, None,         "Inv WSptMin [%]"),
        (44449, U3, 2, "s32", 100,   1, None,         "Bat WSptMax [%]"),
        (44451, U3, 2, "s32", 100,   1, None,         "Bat WSptMin [%]"),
        (44141, U3, 2, "u32", 10000, 0, None,         "cosphi draw PFIn"),
        (44143, U3, 2, "u32", 1,     0, EXC_TAGS,     "cosphi draw PFExtIn"),
        # setpoint channels (ASO) -- unit 2 (plant / System Manager)
        (44493, U2, 2, "s32", 1,    25, None,         "ExtPlntCtl W limit [W]"),
        (40015, U2, 1, "s16", 10,  0.5, None,         "VArNom [%]"),
        (40016, U2, 1, "s16", 1,     0, None,         "WNom [%]  (Node-RED reg!)"),
        (40018, U2, 2, "u32", 1,     0, FSTSTOP_TAGS, "FstStop"),
        (40024, U2, 1, "u16", 10000, 0, None,         "cosphi out PF"),
        (40025, U2, 2, "u32", 1,     0, EXC_TAGS,     "cosphi out PFExt"),
        (40493, U2, 1, "s16", 100, 0.5, None,         "DM WNomPrc [%]"),
        (44429, U2, 2, "s32", 100, 0.5, None,         "DM WNomPrc2 [%]"),
        (41443, U2, 2, "u32", 10,  0.5, None,         "Q(V) VolRef [V]"),
        (44211, U2, 2, "u32", 1000,  0, None,         "Q(V) VolRefPu [p.u.]"),
        (41465, U2, 2, "u32", 1,     0, None,         "Heartbeat LifSignRx"),
        (41535, U2, 2, "s32", 1,    25, None,         "DSO WPCCMin [W]"),
        (41543, U2, 2, "s32", 1,    25, None,         "DSO WDevMin [W]"),
        (41547, U2, 1, "s16", 100, 0.5, None,         "DSO WPCCMinNom [%]"),
        (41549, U2, 1, "s16", 100, 0.5, None,         "DSO WDevMinNom [%]"),
        (41550, U2, 1, "s16", 100, 0.5, None,         "DM WPCCMinNom [%]"),
        # 44577-cascade tripwires (mode parameters; must stay put)
        (44577, U2, 2, "u32", 1, 0, EXTCTL_TAGS, "UseExtPlntCtl  <TRIPWIRE>"),
        (40210, U3, 2, "u32", 1, 0, WMOD_TAGS,   "WMod@dev       <TRIPWIRE>"),
        (41319, U3, 2, "u32", 1, 0, VARMOD_TAGS, "VArModOut@dev  <TRIPWIRE>"),
        (41321, U3, 2, "u32", 1, 0, VARMOD_TAGS, "VArModIn@dev   <TRIPWIRE>"),
        (41323, U3, 2, "u32", 1, 0, VARMOD_TAGS, "VArModZerW@dev <TRIPWIRE>"),
        (41319, U2, 2, "u32", 1, 0, VARMOD_TAGS, "VArModOut@plnt <TRIPWIRE>"),
        (41321, U2, 2, "u32", 1, 0, VARMOD_TAGS, "VArModIn@plnt  <TRIPWIRE>"),
        (41323, U2, 2, "u32", 1, 0, VARMOD_TAGS, "VArModZerW@plnt<TRIPWIRE>"),
        # enforced limits / status (RO)
        (31405, U2, 2, "u32", 1,  10, None,              "Dmd.WCtl enforced [W]"),
        (31245, U2, 2, "u32", 100, 2, None,              "internal PV lim [%]"),
        (31241, U2, 2, "u32", 100, 2, None,              "DM PV lim [%]"),
        (35547, U3, 2, "u32", 1,   0, WSPT_SOURCE_TAGS,  "W-limit source"),
        (30219, U3, 2, "u32", 1,   0, REDUCTION_TAGS,    "reduction reason"),
        (33003, U3, 2, "u32", 1,   0, RUNSTT_TAGS,       "RunStt"),
        (30955, U3, 2, "u32", 1,   0, BAT_OPSTATUS_TAGS, "bat op status"),
        (32503, U2, 2, "u32", 1,   0, None,              "extctl timeout cnt"),
        (32505, U2, 2, "u32", 1,   0, None,              "meter timeout cnt"),
        (32507, U2, 2, "u32", 1,   0, None,              "device timeout cnt"),
    )
    # --focus: only the registers that can flip during a dip, for ~3 Hz sampling
    FOCUS_ADDRS = {41467, 41469, 41431, 41433, 31405, 31245, 35547, 30219,
                   33003, 30955}
    if focus:
        CATALOG = tuple(e for e in CATALOG if e[0] in FOCUS_ADDRS
                        and "<TRIPWIRE>" not in e[7])
    SWEEP_SLEEP = 0.05 if focus else 1.0
    NA = {"s16": -0x8000, "u16": 0xFFFF, "s32": -0x80000000, "u32": 0xFFFFFFFF}

    def is_na(kind, raw):
        # u32 'not available' shows up as several near-0xFFFFFFFF sentinels
        # (observed 2026-07-24: 31245 reads ~4294960000 during EMS maneuvers)
        if kind == "u32":
            return raw >= 0xFFFF0000
        return raw == NA[kind]

    def rd(e):
        addr, unit, count, kind = e[0], e[1], e[2], e[3]
        regs, err = try_read(read_regs, client, addr, count, unit)
        if err is not None:
            return None
        if count == 1:
            raw = regs[0] & 0xFFFF
            if kind == "s16" and raw >= 0x8000:
                raw -= 0x10000
            return raw
        return s32(regs) if kind == "s32" else u32(regs)

    def fmt(e, raw):
        kind, div, tags = e[3], e[4], e[6]
        if raw is None:
            return "ERR"
        if is_na(kind, raw):
            return "n/a"
        if tags is not None:
            return f"{raw}:{tags.get(raw, '?')}"
        return f"{raw / div:g}" if div != 1 else f"{raw:+d}"

    def differs(e, ref, new):
        kind, div, dead, tags = e[3], e[4], e[5], e[6]
        if ref == new:
            return False
        if is_na(kind, ref) and is_na(kind, new):
            return False                 # NaN-variant flapping is not a change
        if tags is not None or dead == 0 or ref is None or new is None:
            return True
        if is_na(kind, ref) or is_na(kind, new):
            return True
        return abs(new - ref) / div > dead

    def key(e):
        return f"{e[0]}@{e[1]}"

    def context():
        bat, _ = try_read(read_battery_w, client)
        pv,  _ = try_read(read_pv_w, client)
        gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
        soc, _ = try_read(read_soc, client)
        return (f"bat={bat[0]:+d}W" if bat else "bat=ERR") + \
               (f"  pv={pv}W" if pv is not None else "  pv=ERR") + \
               (f"  grid={s32(gr):+d}W" if gr else "  grid=ERR") + \
               (f"  SoC={soc}%" if soc is not None else "")

    print("\n=== EMS WATCH (read-only observation of plant-control registers) ===")
    if focus:
        print("FOCUS mode: EMS OFF + esp32 controller RUNNING. Sweeping only the")
        print(f"{len(CATALOG)} dip-relevant registers (~3 Hz). Expect steady churn on")
        print("41467/41469 from our controller; look for VALUES it would never")
        print("write (EMS-style tracking numbers, ~19 s beat). Many ERRs = SBSE")
        print("Modbus connection limit (3 clients incl. Node-RED).")
    else:
        print("Setup (a): internal energy management ON (web UI), esp32 controller")
        print("STOPPED. Node-RED stays RUNNING (40016 falls back to 0% without its")
        print("refresh); expect its steady Dmd.WCtl ~748/0 W and discount it.")
    print(f"This mode performs ZERO writes. Polling for {dur}s;")
    print("Ctrl-C exits cleanly with a summary.\n")

    print("Probing catalog ...")
    watch, dropped, ref = [], [], {}
    for e in CATALOG:
        raw = rd(e)
        if raw is None:
            dropped.append(e)
        else:
            watch.append(e)
            ref[key(e)] = raw
    if dropped:
        print("  not readable (skipped): " +
              ", ".join(f"{e[0]}@{e[1]}" for e in dropped))

    print(f"\nBaseline snapshot ({len(watch)} registers):")
    for e in watch:
        print(f"  {e[0]}@{e[1]}  {e[7]:<28} = {fmt(e, ref[key(e)])}")
    print(f"\n  context: {context()}")
    print("\nWatching (only changes are printed) ...")

    stats = {}   # key -> [n_changes, first_raw, last_raw, min_raw, max_raw]
    start = time.time()
    last_ctx = 0.0
    try:
        while True:
            t = time.time() - start
            if t > dur:
                break
            changes = []
            for e in watch:
                k = key(e)
                raw = rd(e)
                if raw is None:
                    continue                      # transient read error
                if differs(e, ref[k], raw):
                    changes.append((e, ref[k], raw))
                    st = stats.setdefault(k, [0, ref[k], raw, raw, raw])
                    st[0] += 1
                    st[2] = raw
                    if e[6] is None and not is_na(e[3], raw):
                        st[3] = raw if is_na(e[3], st[3]) else min(st[3], raw)
                        st[4] = raw if is_na(e[3], st[4]) else max(st[4], raw)
                    ref[k] = raw
            if changes:
                print(f"\n  t={t:5.0f}s  {context()}")
                for e, old, new in changes:
                    trip = "  !!! TRIPWIRE" if "<TRIPWIRE>" in e[7] else ""
                    print(f"           {e[0]}@{e[1]}  {e[7]:<28} "
                          f"{fmt(e, old)} -> {fmt(e, new)}{trip}")
                last_ctx = t
            elif t - last_ctx >= 30:
                print(f"  t={t:5.0f}s  {context()}  (no register changes)")
                last_ctx = t
            time.sleep(SWEEP_SLEEP)
    except KeyboardInterrupt:
        print("\n(interrupted -- no writes were made, nothing to restore)")

    print("\nChange summary:")
    if not stats:
        print("  no watched register changed. The internal EMS either commands the")
        print("  device outside these channels or nothing happened -- retry during a")
        print("  situation where it must act (surplus with full battery, forced charge).")
    for e in watch:
        k = key(e)
        if k not in stats:
            continue
        n, first, last, lo, hi = stats[k]
        rng = f"  range [{fmt(e, lo)} .. {fmt(e, hi)}]" if e[6] is None else ""
        print(f"  {e[0]}@{e[1]}  {e[7]:<28} {n:4d} changes   "
              f"first {fmt(e, first)} -> last {fmt(e, last)}{rng}")
    print("\nInterpretation:")
    print("  * moving Setpoint.* registers = the internal EMS's actuator set; their")
    print("    values/cadence show HOW it commands the device (compare with the ~19s")
    print("    arbitration beat seen in spikewatch).")
    print("  * Dmd.WCtl (31405) steady at ~748 W (fuel cell on) or ~0 W (off), and")
    print("    flips between the two = Node-RED's 40016 refresh -- expected, ignore.")
    print("  * 31405/31245 moving with stable setpoints = enforcement happens through")
    print("    a channel this catalog cannot see (internal, not Modbus-visible).")
    print("  * any TRIPWIRE line = a mode parameter changed; check events.csv and")
    print("    consider `repair`.")


def cmd_fststop(client, auto_yes):
    """Validate 40018 (FstStop) as the external watchdog's emergency actuator.

    First experiment of the sbse-controller 2.0 project (agreed 2026-07-24):
    before anything touches 44577 again, prove the safety net. 40018 is an ASO
    Setpoint register (unit 2, WO, no flash wear, no parameter cascade), enum
    381 Stop / 1467 Start / 1749 Full stop. The watchdog design fires 381 when
    the ESP32 goes silent; this mode answers, under INTERNAL control:
      1. Is 381 honored at all (and how fast does feed-in stop)?
      2. Does Stop LATCH (persists without re-writes) or decay on its own?
      3. Does 1467 Start bring the inverter back, and how long does the
         restart take (grid-monitoring wait is normal, up to a few minutes)?
    Sequence: baseline 10 s -> write 381 -> observe 60 s (no further writes)
    -> write 1467 -> observe up to 240 s for recovery. On Ctrl-C or error the
    finally block ALWAYS writes 1467 so the inverter is never left stopped.
    EXPECT during Stop: inverter AC power ~0, PV 0, battery idle; the house
    runs on grid (+ fuel cell). Run when a few minutes without PV is fine."""
    STOP_WATCH  = 60
    START_WATCH = 240
    PERIOD = 2
    print("\n=== FSTSTOP TEST (40018 Stop/Start round-trip) ===")
    print("The inverter will STOP FEEDING for ~1 min; PV and battery are offline")
    print("during the test and the house draws from the grid. Fully reversible;")
    print("1467 Start is always written at the end (also on Ctrl-C).")
    if not confirm(auto_yes):
        print("Aborted."); return

    def row(t):
        ac,  _ = try_read(read_s32, client, AC_TOTW_ADDR, UNIT_INVERTER)
        pv,  _ = try_read(read_pv_w, client)
        bat, _ = try_read(read_battery_w, client)
        gr,  _ = try_read(read_regs, client, GRID_POWER_ADDR, 2, UNIT_GRID)
        run, _ = try_read(lambda: u32(read_regs(client, RUNSTT_ADDR, 2, UNIT_INVERTER)))
        rtxt = f"{run}:{RUNSTT_TAGS.get(run, '?')}" if run is not None else "ERR"
        print(f"  {t:4.0f}  {(f'{ac:+5d}W' if ac is not None else 'ERR'):>7}  "
              f"{(f'{pv:5d}W' if pv is not None else 'ERR'):>7}  "
              f"{(f'{bat[0]:+5d}W' if bat else 'ERR'):>7}  "
              f"{(f'{s32(gr):+5d}W' if gr else 'ERR'):>7}   {rtxt}")
        return ac, run

    def watch(label, dur):
        print(f"\n--- {label}")
        print("  t[s]   inv-AC      pv   battery     grid   RunStt(33003)")
        t = 0.0
        while t <= dur:
            time.sleep(PERIOD); t += PERIOD
            row(t)

    try:
        watch("baseline (internal control, nothing written yet)", 10)

        print(f"\n>>> writing 40018 = {FSTSTOP_STOP} (Stop) -- single write, no repeats")
        write_regs(client, FSTSTOP_ADDR, to_s32_regs(FSTSTOP_STOP), UNIT_GRID)
        watch(f"STOPPED? observe {STOP_WATCH}s WITHOUT re-writing (latching test)",
              STOP_WATCH)

        print("\nInterpretation so far:")
        print("  * inv-AC fell to ~0 within seconds and STAYED there = 381 honored")
        print("    and latching -> perfect watchdog actuator.")
        print("  * inv-AC recovered on its own during the window = Stop decays; the")
        print("    watchdog must re-assert 381 cyclically (it may: ASO register).")
        print("  * nothing happened at all = 40018 not honored under internal")
        print("    control; re-test under 44577=Yes in the step-2 experiment.")
    finally:
        print(f"\n>>> writing 40018 = {FSTSTOP_START} (Start) -- releasing the stop")
        try:
            write_regs(client, FSTSTOP_ADDR, to_s32_regs(FSTSTOP_START), UNIT_GRID)
        except Exception as ex:                      # noqa: BLE001
            print(f"  !! Start write FAILED: {ex}")
            print(f"  !! retry manually: write {FSTSTOP_START} to 40018 (unit 2),")
            print("  !! or use the inverter web UI to resume operation.")

    watch(f"RESTART: observe up to {START_WATCH}s (grid-monitoring wait is normal)",
          START_WATCH)
    print("\nNote the seconds from Start to inv-AC > 0 -- that is the recovery time")
    print("the watchdog documentation should state after a trip.")


def cmd_park(client, auto_yes):
    print("\n=== PARK (write WSptMax = WSptMin = 0) ===")
    if not confirm(auto_yes):
        print("Aborted."); return
    write_window(client, 0, 0)
    time.sleep(2)
    snapshot(client, "parked")
    print("Battery released. Re-enable the sbse_controller to resume normal control.")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="SBSE inverter Modbus test tool")
    ap.add_argument("mode", choices=["read", "discharge", "charge", "chargefix",
                                     "chargehold", "chargewatchdog", "chargediag",
                                     "chargewindow", "spikewatch", "extctl",
                                     "extctlhold", "release", "timeout", "repair",
                                     "emswatch", "floorrate", "fststop", "park"])
    ap.add_argument("--ip", default=INVERTER_IP)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--mag", type=int, default=1000, help="test power magnitude [W]")
    ap.add_argument("--settle", type=int, default=8, help="seconds to hold each step")
    ap.add_argument("--dur", type=int, default=120,
                    help="spikewatch/emswatch duration [s] (emswatch: use >= 600)")
    ap.add_argument("--set", type=float, default=None,
                    help="timeout mode: new value for 41525 [s] (omit = read-only)")
    ap.add_argument("--focus", action="store_true",
                    help="emswatch: sweep only dip-relevant registers at ~3 Hz "
                         "(for the EMS-off + controller-running experiment)")
    ap.add_argument("--rates", default="5,1",
                    help="floorrate: comma-separated 41433 refresh periods [s], "
                         "one phase each (e.g. 1,0.3)")
    ap.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    args = ap.parse_args()

    print(f"Connecting to {args.ip}:{args.port} ...")
    client = ModbusTcpClient(args.ip, port=args.port, timeout=3)
    if not client.connect():
        print(f"ERROR: could not connect to {args.ip}:{args.port}")
        sys.exit(1)
    _resolve_unit_kw(client)
    print(f"Connected (pymodbus unit kwarg = {_UNIT_KW!r}).")

    try:
        if args.mode == "read":
            cmd_read(client)
        elif args.mode == "discharge":
            cmd_discharge(client, args.mag, args.settle, args.yes)
        elif args.mode == "charge":
            cmd_charge(client, args.mag, args.settle, args.yes)
        elif args.mode == "chargefix":
            cmd_chargefix(client, args.mag, args.settle, args.yes)
        elif args.mode == "chargehold":
            cmd_chargehold(client, args.mag, args.settle, args.yes)
        elif args.mode == "chargewatchdog":
            cmd_chargewatchdog(client, args.mag, args.settle, args.yes)
        elif args.mode == "chargediag":
            cmd_chargediag(client, args.mag, args.settle, args.yes)
        elif args.mode == "chargewindow":
            cmd_chargewindow(client, args.mag, args.settle, args.yes)
        elif args.mode == "spikewatch":
            cmd_spikewatch(client, args.mag, args.dur, args.yes)
        elif args.mode == "extctl":
            cmd_extctl(client, args.mag, args.dur, args.yes)
        elif args.mode == "extctlhold":
            cmd_extctlhold(client, args.mag, args.dur, args.yes)
        elif args.mode == "release":
            cmd_release(client, args.settle, args.yes)
        elif args.mode == "timeout":
            cmd_timeout(client, args.set, args.yes)
        elif args.mode == "repair":
            cmd_repair(client, args.yes)
        elif args.mode == "emswatch":
            cmd_emswatch(client, args.dur, args.focus)
        elif args.mode == "floorrate":
            try:
                rates = [float(r) for r in args.rates.split(",") if r.strip()]
            except ValueError:
                print(f"ERROR: cannot parse --rates {args.rates!r}"); sys.exit(1)
            if not rates or any(r < 0.05 or r > 60 for r in rates):
                print("ERROR: --rates values must be within 0.05 .. 60 s"); sys.exit(1)
            cmd_floorrate(client, args.mag, args.dur, rates, args.yes)
        elif args.mode == "fststop":
            cmd_fststop(client, args.yes)
        elif args.mode == "park":
            cmd_park(client, args.yes)
    except KeyboardInterrupt:
        if args.mode == "emswatch":
            print("\nInterrupted -- emswatch is read-only, nothing to restore.")
        else:
            print("\nInterrupted -- parking battery (0/0) for safety.")
            try:
                write_window(client, 0, 0)
            except Exception as e:                  # noqa: BLE001
                print(f"  park failed: {e}")
    except Exception as e:                          # noqa: BLE001
        print(f"\nERROR: {e}")
        print("If this is a connection error, the SBSE likely allows only ONE Modbus")
        print("client at a time. Make sure the esp32 sbse_controller is fully stopped")
        print("(not just paused -- it auto-reconnects), wait ~10 s, then retry.")
    finally:
        try:
            client.close()
        except Exception:                           # noqa: BLE001
            pass


if __name__ == "__main__":
    main()
