/** @jsxImportSource preact */
import { h } from "preact";
let x = {
    "sbse_controller": {
        "navbar": {
            "title": "SBSE-Regler"
        },
        "status": {
            "title": "SBSE-Regler",
            "grid": "Netz",
            "battery": "Batterie",
            "soc": "Ladezustand",
            "setpoint": "Sollwert",
            "last_write": "Letzter Schreibvorgang",
            "write_errors": "Schreibfehler",
            "grid_target_range":      "Netz-Zielbereich",
            "grid_target_range_help": <>Das <code>[unten, oben]</code> Netz-Totband. Der linke Griff ist <code>grid_charge_target_w</code> (Regler lädt die Batterie, damit der Netzwert nicht darunter fällt). Der rechte Griff ist <code>grid_discharge_target_w</code> (Regler entlädt, damit der Netzwert nicht darüber steigt). Beide Griffe auf den <em>gleichen</em> Wert = harte Einzielregelung; auseinander = weiches Totband, in dem die Batterie zwischen den beiden Werten ruht. Sofortige Übernahme ohne Flash-Schreibvorgang.</>,
            "max_charge_w":  "Max. Ladeleistung",
            "max_charge_w_help":  <>Obergrenze für die Batterieladeleistung. Sofortige Übernahme ohne Flash-Schreibvorgang. <code>0</code> verbietet das Laden.</>,
            "max_discharge_w": "Max. Entladeleistung",
            "max_discharge_w_help": <>Obergrenze für die Batterieentladeleistung. Sofortige Übernahme ohne Flash-Schreibvorgang. <code>0</code> verbietet das Entladen.</>,
            "apply":  "Übernehmen",
            "pause":  "30 s pausieren (0 W)",
            "resume": "Fortsetzen",
            "mb_badge":  "MB",
            "mb_badge_help_title": "Ein externer Modbus-TCP-Client steuert den Regler aktuell",
            "read_led_help_title": "Blinkt bei jedem über den SMA-Hybrid-Proxy beantworteten Modbus-Lesezugriff",
            "hard_badge": "HART",
            "hard_badge_help_title": "Harter Zielwert: Lade- und Entlade-Schwelle sind identisch; der Regler verfolgt diesen einen Netzwert in beide Richtungen.",
            "soft_badge": "WEICH",
            "soft_badge_help_title": "Weicher Zielwert: Lade- und Entlade-Schwelle unterscheiden sich. Die Batterie ruht im Netz-Totband [Lade, Entlade]; außerhalb verfolgt der Regler die nähere Grenze.",

            "mode_disabled":        "deaktiviert",
            "mode_not_connected":   "nicht verbunden",
            "mode_stale":           "veraltet",
            "mode_running":         "läuft",
            "mode_paused":          "pausiert",
            "mode_safety":          "Sicherheitsmodus",
            "mode_faulted":         "Fehler",
            "mode_force_charge":    "Zwangsladung",
            "mode_force_discharge": "Zwangsentladung",
            "mode_blocked":         "gesperrt",
            "mode_block_charge":    "Laden gesperrt",
            "mode_block_discharge": "Entladen gesperrt"
        },
        "chart": {
            "grid":      "Netz (EMA)",
            "battery":   "Batterie",
            "setpoint":  "Sollwert",
            "target_lo": "Lade-Schwelle",
            "target_hi": "Entlade-Schwelle",
            "time":      "Zeit",
            "no_data":   "Noch keine Messwerte",
            "loading":   "Sammle Messwerte…"
        },
        "content": {
            "title": "SBSE-Regler",

            "section_connection": "Verbindung",
            "section_timing":     "Regelzyklus",
            "section_targets":    "Sollwerte",
            "section_tuning":     "Reglerparameter",
            "section_safety":     "Sicherheit",

            "enabled":         "Aktiviert",
            "enabled_desc":    "Sollwertregler ausführen",
            "enabled_help":    <>Hauptschalter. Eine Änderung wird erst nach einem Neustart wirksam (der Netzwerk-verbunden-Event, der die TCP-Verbindung startet, wurde bereits zugestellt).</>,
            "enabled_label":   "ein",
            "disabled_label":  "aus",

            "host":      "Wechselrichter-Host",
            "host_help": <>IP-Adresse oder Hostname des SBSE-Modbus-TCP-Gateways. Eine Änderung erfordert einen Neustart.</>,
            "port":      "Modbus-TCP-Port",
            "port_help": <>TCP-Port des SBSE-Modbus-Gateways. Eine Änderung erfordert einen Neustart.</>,

            "tick_ms":      "Zyklusdauer",
            "tick_ms_help": <>Zykluszeit der Regelung. Ein Lesen–Berechnen–Schreiben pro Zyklus. Kürzer = schnellere Netznachführung, mehr Modbus-Verkehr. Eine Änderung erfordert einen Neustart.</>,

            "soc_interval_ms":      "SoC-Abfrageintervall",
            "soc_interval_ms_help": <>Wie oft der Batterie-Ladezustand gelesen wird. Der SoC ändert sich langsam; häufige Abfragen sind verschwendete Bandbreite. Muss ≥ Zyklusdauer sein. Eine Änderung erfordert einen Neustart.</>,

            "grid_charge_target_w":      "Lade-Schwelle (untere Netzgrenze)",
            "grid_charge_target_w_help": <>Untere Grenze des Netz-Totbands. Würde der Netzwert unter diesen Wert fallen (z. B. PV-Überschuss über dem konfigurierten Einspeise-Zielwert), lädt der Regler die Batterie und hebt den Netzwert wieder an. Identisch zur Entlade-Schwelle = harte Einzielregelung; niedriger gesetzt (z. B. <code>-200 W</code>) erzeugt ein asymmetrisches Totband im Weich-Modus. Typischer Wert für Eigenverbrauch: <code>0 W</code>.</>,

            "grid_discharge_target_w":      "Entlade-Schwelle (obere Netzgrenze)",
            "grid_discharge_target_w_help": <>Obere Grenze des Netz-Totbands. Würde der Netzwert über diesen Wert steigen (d. h. Bezug), entlädt der Regler die Batterie und senkt den Netzwert wieder. Muss ≥ Lade-Schwelle sein. Typische Werte: <code>0 W</code> (kein autonomer Netzbezug, die Batterie deckt jegliches Defizit ab) für Eigenverbrauch mit „kein Bezug"-Präferenz; identisch zur Lade-Schwelle für harte Einzielregelung; größerer Wert, um Bezug bis zu dieser Grenze zuzulassen, bevor die Batterie entlädt.</>,

            "max_charge_w":      "Max. Ladeleistung",
            "max_charge_w_help": <>Obergrenze für die Batterieladeleistung. 0 verbietet das Laden vollständig.</>,

            "max_discharge_w":      "Max. Entladeleistung",
            "max_discharge_w_help": <>Obergrenze für die Batterieentladeleistung. 0 verbietet das Entladen vollständig.</>,

            "kp":      "P-Verstärkung (Kp)",
            "kp_help": <>Proportionale Verstärkung des Reglers. <code>neuer_sollwert = batterie_jetzt + Kp · (ema_netz − ziel) + Kd · Δema_netz</code>. Niedriger = ruhiger; höher = schneller, aber kann schwingen.</>,

            "kd":      "D-Verstärkung (Kd)",
            "kd_help": <>Differentielle Verstärkung, berechnet aus der geglätteten Netzleistung (nicht aus dem Regelfehler – Sollwertänderungen erzeugen daher keinen D-Impuls). <code>0</code> deaktiviert den D-Anteil. Erhöhen, wenn schnelle Lastsprünge (Induktionsfeld, Wasserkocher) den P-Regler überschwingen lassen, bevor das Netz-EMA nachgezogen hat. Faustregel: bei <code>1,0</code> starten; auf <code>2,0</code>–<code>3,0</code> erhöhen, wenn das Überschwingen bleibt; reduzieren, wenn die Kurve zu schwingen beginnt.</>,

            "alpha_grid":      "Glättung Netz-Eingang (α)",
            "alpha_grid_help": <>EMA-Faktor für die Netzleistungsmessung. <code>α=1,0</code> = keine Glättung; kleinere Werte filtern stärker, dafür mit Verzögerung.</>,

            "alpha_setpoint":      "Glättung Sollwert (α)",
            "alpha_setpoint_help": <>EMA-Faktor auf den an den Wechselrichter geschickten Sollwert. Verhindert flackernde Kommandos aus rauschenden Messwerten.</>,

            "deadband_w":      "Schreib-Totband",
            "deadband_w_help": <>Liegt der neu berechnete Sollwert innerhalb ±Totband um den zuletzt geschriebenen Wert, wird der Modbus-Schreibvorgang übersprungen. Der SBSE hält den letzten Wert; großzügige Totbänder sind unkritisch.</>,

            "safety_zero_after_failures":      "Sicherheits-Null nach N Lesefehlern",
            "safety_zero_after_failures_help": <>Nach so vielen aufeinanderfolgenden Lesefehlern setzt der Regler einmalig den Sollwert auf 0 W und bleibt im Sicherheitsmodus, bis das Lesen wieder funktioniert. Auf 0 setzen, um diese Sicherung zu deaktivieren.</>,

            "keepalive_interval_s":      "Keepalive- / Schreib-Watchdog-Intervall",
            "keepalive_interval_s_help": <>Begrenzt die maximale Zeit zwischen Modbus-Schreibvorgängen auf zweierlei Weise: <strong>(1) Leerlauf-Impuls</strong> — war die Batterie so lange inaktiv, sendet der Regler einen kleinen alternierenden ±<code>keepalive_pulse_w</code>-Impuls, damit der SBSE nicht in seinen ~10–15 min-Standby fällt (Aufwachverzögerung ~20–30 s); <strong>(2) Aktive Auffrischung</strong> — ist die Batterie aktiv, der Sollwert aber länger als das Intervall innerhalb des Schreib-Totbands stabil (stetige Regelung, Force-Modus mit konstantem Wert), schreibt der Regler den aktuellen Sollwert trotzdem erneut. Standard 480 s = 8 min liegt komfortabel unterhalb der Standby-Schwelle. Deaktivieren, um den Wechselrichter regulär in Standby gehen zu lassen – sinnvoll bei niedrigen Leerlaufverlusten, wenn die Anlaufverzögerung egal ist.</>,

            "keepalive_pulse_w":      "Keepalive-Impulshöhe",
            "keepalive_pulse_w_help": <>Wie groß jeder Keepalive-Impuls ist. Die Impulsrichtung wechselt sich zwischen aufeinanderfolgenden Auslösungen ab, sodass der langfristige Energiebeitrag im Mittel null ergibt. Jeder Impuls dauert genau einen Tick (typ. 300 ms); die tatsächlich bewegte Energie liegt im Millijoule-Bereich – weit unterhalb der Rauschschwelle einer Zelle. Der Standardwert <code>50 W</code> ist groß genug, um vom Wechselrichter als „Batterie aktiv" registriert zu werden, aber im Netz unsichtbar. Auf <code>0</code> setzen, um Keepalive zu deaktivieren, ohne das Intervall zu verändern.</>,

            "section_modbus_server":           "Modbus-TCP-Server (externe Steuerung)",
            "modbus_server_enabled":           "Modbus-TCP-Server",
            "modbus_server_enabled_desc":      "SMA-kompatible Sollwert-Schreibvorgänge entgegennehmen",
            "modbus_server_enabled_help":      <>Startet einen Modbus-TCP-Server, der dieselben WriteMultipleRegisters-Befehle akzeptiert, die das WARP-Charger-Modul „SMA Hybrid Inverter" an einen echten Sunny Boy Storage sendet. Block, Normal, Block Entladen, Block Laden, Zwangsladung und Zwangsentladung werden unterstützt. Jeder Schreibvorgang aktualisiert auch das laufende <code>active_config</code>, sodass das Dashboard widerspiegelt, was der externe Client tut. Bedienereingriffe über Dashboard / HTTP / MQTT übernehmen sofort („letzter Schreibvorgang gewinnt"). Eine Portänderung erfordert einen Neustart.</>,
            "modbus_server_port":              "Port",
            "modbus_server_port_help":         <>TCP-Port, auf dem gelauscht wird. SMA-Standard ist <code>502</code>. Eine Änderung erfordert einen Neustart.</>,
            "modbus_server_unit_id":           "Unit-ID",
            "modbus_server_unit_id_help":      <>Modbus-Unit-ID, auf die dieser Server antwortet. Der Standardwert <code>3</code> entspricht der SMA-Hybrid-Wechselrichter-Konvention, sodass bestehende Clients ohne Änderung funktionieren. Auf <code>0</code> setzen, um jede Unit-ID zu akzeptieren.</>,
            "modbus_server_watchdog_s":        "Watchdog-Timeout",
            "modbus_server_watchdog_s_help":   <>Trifft innerhalb dieser Anzahl Sekunden kein Modbus-Schreibvorgang ein, setzt der Regler die Live-Überschreibungen (<code>target_grid_w</code>, <code>max_charge_w</code>, <code>max_discharge_w</code>) auf die persistenten Konfigurationswerte zurück und verlässt den Force-Modus. Der echte SMA-Wechselrichter-Watchdog läuft nach 5 Minuten ab; der Standard von 60 s entspricht dem Sendeintervall des WARP-Chargers. Deaktivieren, um den letzten Sollwert dauerhaft zu halten.</>,
            "modbus_server_authority":            "Modbus-Befugnis",
            "modbus_server_authority_help":       <>Wie viel der vom Bediener konfigurierten <code>active_config</code> darf ein externer Modbus-Client bei jedem WriteMultipleRegisters auf Register 40793 überschreiben. Die SMA-<code>OpMod</code>-Force-Befehle (2289 / 2290) werden stets ausgeführt; diese Einstellung steuert nur die persistenten Caps und Netz-Zielwerte.</>,
            "modbus_server_authority_force_only": "Nur Force-Befehle — alle eigenen Einstellungen schützen",
            "modbus_server_authority_caps":       "Caps — max_charge_w / max_discharge_w übernehmen (Standard)",
            "modbus_server_authority_full":       "Voll — zusätzlich GridWSpt in beide Netz-Zielwerte übernehmen"
        },
        "script": {
            "save_failed":          "Speichern der SBSE-Regler-Einstellungen fehlgeschlagen",
            "save_active_failed":   "Übernahme des Live-Sollwerts fehlgeschlagen",
            "pause_failed":        "Pause des Reglers fehlgeschlagen",
            "resume_failed":        "Fortsetzen des Reglers fehlgeschlagen"
        }
    }
}
