/** @jsxImportSource preact */
import { h } from "preact";
let x = {
    "sbse_controller": {
        "navbar": {
            "title": "SBSE-Regler"
        },
        "status": {
            "title": "SBSE 5.0 Regler",
            "grid": "Netz",
            "battery": "Batterie",
            "soc": "Ladezustand",
            "setpoint": "Sollwert",
            "last_write": "Letzter Schreibvorgang",
            "write_errors": "Schreibfehler",
            "target_grid_w": "Ziel-Netzaustausch",
            "target_grid_w_help": <>Positiv = Netzbezug. Negativ = Einspeisung. Sofortige Übernahme ohne Flash-Schreibvorgang.</>,
            "max_charge_w":  "Max. Ladeleistung",
            "max_charge_w_help":  <>Obergrenze für die Batterieladeleistung. Sofortige Übernahme ohne Flash-Schreibvorgang. <code>0</code> verbietet das Laden.</>,
            "max_discharge_w": "Max. Entladeleistung",
            "max_discharge_w_help": <>Obergrenze für die Batterieentladeleistung. Sofortige Übernahme ohne Flash-Schreibvorgang. <code>0</code> verbietet das Entladen.</>,
            "apply": "Übernehmen",
            "force_release": "30 s pausieren (0 W)",
            "resume":        "Fortsetzen",
            "sim_badge": "SIM",
            "mb_badge":  "MB",
            "mb_badge_help_title": "Ein externer Modbus-TCP-Client steuert den Regler aktuell",

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
            "grid":     "Netz (EMA)",
            "battery":  "Batterie",
            "setpoint": "Sollwert",
            "target":   "Ziel",
            "time":     "Zeit",
            "no_data":  "Noch keine Messwerte",
            "loading":  "Sammle Messwerte…"
        },
        "content": {
            "title": "SBSE 5.0 Regler",

            "section_connection": "Verbindung",
            "section_mode":       "Betriebsmodus",
            "section_timing":     "Regelzyklus",
            "section_targets":    "Sollwerte",
            "section_tuning":     "Reglerparameter",
            "section_safety":     "Sicherheit",

            "simulation_mode":      "Simulationsmodus",
            "simulation_mode_desc": "Regler laufen lassen, ohne in den Wechselrichter zu schreiben",
            "simulation_mode_help": <>Wenn aktiviert, liest und berechnet der Regler weiterhin Netz/Batterie/SoC und Sollwerte wie im Normalbetrieb, überspringt aber den eigentlichen Modbus-Schreibvorgang. Nützlich, um die Reglerparameter zu prüfen, bevor die Batterie tatsächlich angesteuert wird. Das Live-Diagramm, der Statuszustand und die Totband-Logik verhalten sich, als wären die Schreibvorgänge erfolgt.</>,

            "enabled":         "Aktiviert",
            "enabled_desc":    "Sollwertregler ausführen",
            "enabled_help":    <>Hauptschalter. Eine Änderung wird erst nach einem Neustart wirksam (der Netzwerk-verbunden-Event, der die TCP-Verbindung startet, wurde bereits zugestellt).</>,
            "enabled_label":   "ein",
            "disabled_label":  "aus",

            "host":      "Wechselrichter-Host",
            "host_help": <>IP-Adresse oder Hostname des SBSE-Modbus-TCP-Gateways.</>,
            "port":      "Modbus-TCP-Port",

            "tick_ms":      "Zyklusdauer",
            "tick_ms_help": <>Zykluszeit der Regelung. Ein Lesen–Berechnen–Schreiben pro Zyklus. Kürzer = schnellere Netznachführung, mehr Modbus-Verkehr.</>,

            "soc_interval_ms":      "SoC-Abfrageintervall",
            "soc_interval_ms_help": <>Wie oft der Batterie-Ladezustand gelesen wird. Der SoC ändert sich langsam; häufige Abfragen sind verschwendete Bandbreite. Muss ≥ Zyklusdauer sein.</>,

            "target_grid_w":      "Ziel-Netzaustauschleistung",
            "target_grid_w_help": <>Auf welchen Wert die Netzleistung geregelt wird. Positiv = Netzbezug; negativ = Einspeisung. 0 = reiner Eigenverbrauch.</>,

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
            "modbus_server_use_grid_spt":      "Modbus-GridWSpt als Zielwert verwenden",
            "modbus_server_use_grid_spt_desc": "Modbus-Schreibvorgänge überschreiben den konfigurierten Zielwert",
            "modbus_server_use_grid_spt_help": <>Wenn <strong>aus</strong> (Standard), aktualisieren Modbus-Schreibvorgänge auf Register 40793 nur <code>max_charge_w</code> und <code>max_discharge_w</code>; der vom Bediener konfigurierte <code>target_grid_w</code> bleibt erhalten. Das entspricht dem Verhalten der „SMA Hybrid Inverter"-Batterieklasse des WARP-Chargers, die unabhängig vom Modus stets <code>GridWSpt = 0</code> sendet -- bei aktivierter Option würde das den Zielwert bei jedem Moduswechsel auf 0 setzen. Wenn <strong>ein</strong>, übernimmt jeder Modbus-Schreibvorgang das <code>GridWSpt</code>-Feld auch in <code>active_config.target_grid_w</code>. Für Clients aktivieren, die das Grid-Setpoint-Feld tatsächlich nutzen.</>
        },
        "script": {
            "save_failed":          "Speichern der SBSE-Regler-Einstellungen fehlgeschlagen",
            "save_active_failed":   "Übernahme des Live-Sollwerts fehlgeschlagen",
            "force_release_failed": "Auslösen von force_release fehlgeschlagen",
            "resume_failed":        "Fortsetzen des Reglers fehlgeschlagen"
        }
    }
}
