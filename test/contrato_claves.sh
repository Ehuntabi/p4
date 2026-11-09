#!/usr/bin/env bash
#
# contrato_claves.sh — comprueba EN LA PLACA que cada endpoint pide la clave que
# tiene que pedir. Es el contrato VIVO entre el firmware y la app.
#
# POR QUE EXISTE (09-nov-2026). El dueño abrio "Logs batería" en la app y salio
# "El P4 no acepta la clave (responde 401)" con la clave buena: la app mandaba
# la clave de ADMINISTRACION a /data/*.csv, que solo aceptaba la del PORTAL. La
# verdad de "que clave pide cada endpoint" vivia en tres sitios que nadie
# comparaba (las macros del firmware, el codigo de la app y los comentarios), y
# la auditoria (test/auditar.sh seccion 14) solo puede mirar el codigo. Esto
# mira la placa: recorre la tabla, la pide con las DOS claves y comprueba que
# cada una da lo que toca.
#
# LA REGLA (revisada por el arbitraje adversarial del 09-nov-2026):
#   - DESCARGAS DE DATOS (los .csv, el HTML de datos y los .tar de frigo,
#     bateria, solar, historico, viaje, capturas, vigilancia y logs): las DOS
#     claves valen. La app manda la del portal. OJO: esto SI amplia lo que se
#     baja con la clave del portal (capturas, vigilancia y logs dejan de pedir
#     la de admin); es decision del dueño, unico usuario.
#   - EXCEPCION: config.tar (el backup) exige la de ADMINISTRACION, porque el
#     fichero que hay en la tarjeta puede ser de una version anterior y llevar
#     las claves AES (el de esta placa las lleva).
#   - ESTADO Y LISTADOS que consulta la app (/api/state, /api/pantalla/info,
#     /data/viajes): nivel PORTAL -- solo la del portal. No se abren a la de
#     admin porque no hace falta (la app manda la del portal).
#   - ESCRITURA DE FIRMWARE Y SECRETOS (/ota, /api/pantalla/subir, /keys,
#     /save): SOLO la de administracion. /api/pantalla/subir contesta 403 (no
#     401) a la clave del portal: es "estas dentro pero no puedes", y el
#     firmware lo hace a proposito para que el navegador no vuelva a pedir una
#     clave que ya vale.
#
# NO TOCA NADA: solo GETs y tres POST (/ota y /api/pantalla/subir **sin cuerpo**
# con la clave del portal, y /save vacio, que tambien rebotan en la puerta de
# autenticacion antes de mirar el cuerpo). Con la clave de admin NO se prueba
# /ota: un POST a /ota es, por definicion, una grabacion, y un test de banco no
# debe tocar la flash.
#
# Uso:
#   PORTAL_PASS=<clave del portal> ADMIN_PASS=<clave de admin> \
#       bash test/contrato_claves.sh [IP]
#
#   Los usuarios aqui son los de fabrica: p4 (portal, se puede cambiar en la
#   pantalla) y portal (administracion). Las claves NO se escriben en el repo
#   (es publico): se pasan por entorno o por el fichero ~/.dsh/claves-p4 si
#   existe (PORTAL_PASS=... / ADMIN_PASS=...). En una placa SIN clave de
#   administracion, las filas estrictas dan 200 con la del portal (el firmware
#   cae a ella): es normal y no es un fallo.
#
set -uo pipefail

HOST="${1:-192.168.4.1}"
PORTAL_USER=p4
ADMIN_USER=portal

if [ -f "$HOME/.dsh/claves-p4" ]; then
    # shellcheck disable=SC1091
    . "$HOME/.dsh/claves-p4"
fi
if [ -z "${PORTAL_PASS:-}" ] || [ -z "${ADMIN_PASS:-}" ]; then
    echo "Faltan las claves. Uso:"
    echo "  PORTAL_PASS=... ADMIN_PASS=... bash test/contrato_claves.sh [IP]"
    exit 2
fi

fallos=0
ok()  { printf '  [ok]   %s\n' "$*"; }
mal() { printf '  [!!]   %s\n' "$*"; fallos=$((fallos + 1)); }

# El firmware frena por IP los fallos de autenticacion seguidos (3 gratis y
# luego espera): encadenar muchos 401/403 esperados devolveria 429, que NO seria
# un fallo del firmware. Un acierto limpia el contador (auth_ip_limpia), asi que
# antes de cada caso que espera un rechazo se hace una peticion buena.
limpia_frenos() {
    curl -s -o /dev/null -u "$PORTAL_USER:$PORTAL_PASS" --max-time 10 \
         "http://$HOST:80/api/state" || true
}

# vale <codigo> <lista separada por |>
vale() {
    case "|$2|" in *"|$1|"*) return 0 ;; esac
    return 1
}

# caso <metodo> <ruta> <puerto> <esperado portal> <esperado admin> [nota]
caso() {
    local m=$1 ruta=$2 puerto=$3 ep=$4 ea=$5 nota="${6:-}"
    local url="http://$HOST:$puerto$ruta" cp ca
    cp=$(curl -s -o /dev/null -w '%{http_code}' -X "$m" \
              -u "$PORTAL_USER:$PORTAL_PASS" --max-time 90 "$url")
    ca=$(curl -s -o /dev/null -w '%{http_code}' -X "$m" \
              -u "$ADMIN_USER:$ADMIN_PASS" --max-time 90 "$url")
    if vale "$cp" "$ep" && vale "$ca" "$ea"; then
        ok "$(printf '%-50s' "$m $ruta (:$puerto)") portal=$cp admin=$ca ${nota}"
    else
        mal "$(printf '%-50s' "$m $ruta (:$puerto)") portal=$cp (esperado $ep) admin=$ca (esperado $ea) ${nota}"
    fi
}

echo "== Contrato de claves contra $HOST =="

echo "-- 1) Descargas de datos: las dos claves valen --"
# El puerto 80 es el principal y el 8081 el "pesado" (los .tar, los csv y la
# galeria: PORTAL_HEAVY_BASE en config_server_internal.h).
caso GET /data                 80     "200|503" "200|503" "(indice de datos)"
caso GET /data/bateria         8081   "200|503" "200|503" "(pagina de la grafica)"
caso GET /data/frigo           8081   "200|503" "200|503" "(pagina de la grafica)"
caso GET /data/bateria.csv   8081   "200|503" "200|503" "(Logs bateria de la app)"
caso GET /data/frigo.csv     8081   "200|503" "200|503"
caso GET /data/ne185v.csv    8081   "200|503" "200|503"
caso GET /data/viaje.tar?v=no_existe 8081 "404|503" "404|503" "(la clave pasa; el viaje no existe)"
caso GET /data/frigo.tar     8081   "200|503" "200|503"
caso GET /data/bateria.tar   8081   "200|503" "200|503"
caso GET /data/solar.tar     8081   "200|503" "200|503"
caso GET /data/capturas.tar  8081   "200|503" "200|503"
caso GET /data/logs.tar      8081   "200|500" "200|500"
caso GET /data/historico.tar 8081   "200|503" "200|503"
# La galeria puede estar ocupada por la camara (503 = "ahora no", no un 401).
caso GET /data/vigilancia.tar 8081  "200|503" "200|503" "(la camara puede estar ocupada)"

echo "-- 2) Estado y listados de la app: nivel PORTAL --"
caso GET /api/state          80     200 401 "(la app manda la del portal)"
caso GET /api/pantalla/info  80     200 401
caso GET /data/viajes        80     200 401

echo "-- 3) Secretos y firmware: SOLO la de administracion --"
caso GET /data/config.tar    8081   401 "200|404" "(backup; 404 = no hay respaldo)"
caso GET /keys               80     401 200 "(claves AES en crudo)"
caso GET /api/perf           80     401 200 "(numeros de rendimiento)"

# Con la clave del PORTAL, lo que escribe firmware tiene que rebotar ANTES de
# mirar el cuerpo. NO se prueba con la de admin: un POST a /ota es una
# grabacion y un test de banco no debe tocar la flash.
solo_portal() {
    local m=$1 ruta=$2 puerto=$3 esperado=$4 nota=${5:-}
    local c
    limpia_frenos
    c=$(curl -s -o /dev/null -w '%{http_code}' -X "$m" -u "$PORTAL_USER:$PORTAL_PASS" \
              --max-time 30 "http://$HOST:$puerto$ruta")
    if vale "$c" "$esperado"; then
        ok "$(printf '%-50s' "$m $ruta (clave del portal)") $c ${nota}"
    else
        mal "$(printf '%-50s' "$m $ruta (clave del portal)") da $c, deberia dar $esperado: la clave del portal no puede escribir firmware"
    fi
}
solo_portal POST /ota               8081 401 "(sin cuerpo: 401 = ni lo mira)"
solo_portal POST /api/pantalla/subir 8081 403 "(403 = estas dentro pero no puedes)"
solo_portal POST /save              80   "401|403"

echo "-- 4) Sin clave no se entra --"
for r in "/api/state:80" "/data/bateria.csv:8081" "/data/config.tar:8081" "/data/logs.tar:8081"; do
    ruta=${r%:*}; puerto=${r#*:}
    limpia_frenos
    c=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "http://$HOST:$puerto$ruta")
    if [ "$c" = "401" ]; then
        ok "$(printf '%-50s' "GET $ruta (:$puerto) sin cabecera") 401"
    else
        mal "$(printf '%-50s' "GET $ruta (:$puerto) sin cabecera") da $c, deberia dar 401"
    fi
done

echo
if [ "$fallos" -eq 0 ]; then
    echo "CONTRATO DE CLAVES OK"
else
    echo "CONTRATO DE CLAVES CON FALLOS: $fallos"
    exit 1
fi
