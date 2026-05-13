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
            "apply": "Übernehmen",
            "force_release": "30 s pausieren (0 W)",

            "mode_disabled":      "deaktiviert",
            "mode_not_connected": "nicht verbunden",
            "mode_stale":         "veraltet",
            "mode_running":       "läuft",
            "mode_paused":        "pausiert",
            "mode_safety":        "Sicherheitsmodus",
            "mode_faulted":       "Fehler"
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
            "section_timing":     "Regelzyklus",
            "section_targets":    "Sollwerte",
            "section_tuning":     "Reglerparameter",
            "section_safety":     "Sicherheit",
            "section_chart":      "Live-Verlauf (letzte 5 Minuten)",

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
            "kp_help": <>Proportionale Verstärkung des Reglers. <code>neuer_sollwert = batterie_jetzt + Kp · (ema_netz − ziel)</code>. Niedriger = ruhiger; höher = schneller, aber kann schwingen.</>,

            "alpha_grid":      "Glättung Netz-Eingang (α)",
            "alpha_grid_help": <>EMA-Faktor für die Netzleistungsmessung. <code>α=1,0</code> = keine Glättung; kleinere Werte filtern stärker, dafür mit Verzögerung.</>,

            "alpha_setpoint":      "Glättung Sollwert (α)",
            "alpha_setpoint_help": <>EMA-Faktor auf den an den Wechselrichter geschickten Sollwert. Verhindert flackernde Kommandos aus rauschenden Messwerten.</>,

            "deadband_w":      "Schreib-Totband",
            "deadband_w_help": <>Liegt der neu berechnete Sollwert innerhalb ±Totband um den zuletzt geschriebenen Wert, wird der Modbus-Schreibvorgang übersprungen. Der SBSE hält den letzten Wert; großzügige Totbänder sind unkritisch.</>,

            "safety_zero_after_failures":      "Sicherheits-Null nach N Lesefehlern",
            "safety_zero_after_failures_help": <>Nach so vielen aufeinanderfolgenden Lesefehlern setzt der Regler einmalig den Sollwert auf 0 W und bleibt im Sicherheitsmodus, bis das Lesen wieder funktioniert. Auf 0 setzen, um diese Sicherung zu deaktivieren.</>
        },
        "script": {
            "save_failed":          "Speichern der SBSE-Regler-Einstellungen fehlgeschlagen",
            "save_active_failed":   "Übernahme des Live-Sollwerts fehlgeschlagen",
            "force_release_failed": "Auslösen von force_release fehlgeschlagen"
        }
    }
}
