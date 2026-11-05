#!/usr/bin/env bash
# rearmar_simulador.sh — deja el puente emitiendo los 3 Victron simulados.
# HACE FALTA CADA VEZ QUE SE ABRE EL PUERTO SERIE DEL PUENTE (abrir el puerto lo
# reinicia y el simulador se pierde; la P4 se queda sin datos y TODAS las
# tarjetas enseñan "--").

#!/bin/bash
cd /home/jc/joint/p4/tools/puente_pruebas

# ── LAS CLAVES, FUERA DEL REPO (05-nov-2026, auditoria) ─────────────────────
# Aqui estaban escritas la clave del AP y la del portal, y este fichero SI va al
# repositorio: cualquiera que lo leyera tenia las dos. Ahora se cogen del entorno
# o de un fichero local que no se versiona (puente_claves.env, en .gitignore).
if [ -f ./puente_claves.env ]; then . ./puente_claves.env; fi
: "${PUENTE_AP_CLAVE:?Falta PUENTE_AP_CLAVE: exportala o ponla en tools/puente_pruebas/puente_claves.env}"
: "${PUENTE_PORTAL_CLAVE:?Falta PUENTE_PORTAL_CLAVE: exportala o ponla en tools/puente_pruebas/puente_claves.env}"
: "${PUENTE_SSID:=P4Config}"
export PUENTE_AP_CLAVE PUENTE_PORTAL_CLAVE PUENTE_SSID
python3 -u - <<'PY'
import json, sys, time
sys.path.insert(0,'.')
from puente import Puente
p = Puente(); p.sincronizar()
print("[1] puente reiniciado y consola lista", flush=True)
# SSID = el que tenga la placa en su NVS ("VictronConfig" en las instaladas,
# "P4Config" en una nueva de fabrica).
p.esperar_ok("wifista %s %s" % (__import__("os").environ.get("PUENTE_SSID", "P4Config"),
                                  __import__("os").environ["PUENTE_AP_CLAVE"]), veces=2, espera=25)
print("[2] asociado al AP de la P4", flush=True)
devs=[d for d in json.load(open("/tmp/devices_victron.json")) if d.get("nombre")][:3]
orden=("sim ble "+" ".join(f"{d['mac']} {d['key_hex']}" for d in devs))[:250]
for c in (orden,"sim ritmo 50","sim caos on"):
    ok,_=p.esperar_ok(c, veces=3, espera=12)
    print(f"[3] {c[:24]:24} {ok}", flush=True)
print("[4] estado:", [l for l in p.cmd("sim", espera=10) if l.startswith("SIM")], flush=True)
p.cerrar(); print("[5] listo: el simulador queda emitiendo", flush=True)
PY
