#!/usr/bin/env bash
# test/portal_actividad.sh — Que peticiones cuentan como "hay alguien dentro"
# del portal y cuales son trafico de las cabinas.
#
# POR QUE EXISTE (23-nov-2026): el icono de Wi-Fi de la P4 se pone azul cuando
# alguien mira el portal, y para eso hay que separar al que mira de las cabinas,
# que preguntan solas (apuntes del cuaderno, callar alarmas y la comprobacion de
# firmware cada 60 s). Si esa lista se queda corta al anadir un canal nuevo, el
# icono se quedaria azul para siempre con el vehiculo parado -- un aviso que
# miente todo el dia es peor que no tenerlo, y en la placa no se nota.
#
# Igual que los demas tests de este directorio: NO hay una copia de la funcion.
# Se extrae del fuente REAL (main/portal/config_server.c) entre sus marcadores
# "INICIO/FIN BLOQUE PRUEBA" y se compila con el gcc de la maquina.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=main/portal/config_server.c
[ -f "$SRC" ] || { echo "FALLO: no encuentro $SRC"; exit 1; }

grep -q "INICIO BLOQUE PRUEBA portal_uri_es_de_maquina" "$SRC" \
    || { echo "FALLO: falta el marcador INICIO del bloque de portal_uri_es_de_maquina"; exit 1; }
grep -q "FIN BLOQUE PRUEBA portal_uri_es_de_maquina" "$SRC" \
    || { echo "FALLO: falta el marcador FIN del bloque de portal_uri_es_de_maquina"; exit 1; }

# Y los macros de auth tienen que seguirlo llamando: si alguien los toca y se
# deja la llamada, el icono se queda verde siempre y aqui no se entera nadie.
for macro in REQUIRE_AUTH REQUIRE_AUTH_STRICT REQUIRE_AUTH_PORTAL_O_ADMIN; do
    grep -A3 "define $macro(req)" main/portal/config_server_internal.h \
        | grep -q "portal_actividad(req)" \
        || { echo "FALLO: $macro ya no llama a portal_actividad()"; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$SRC" "$TMP/caso.c" <<'PY'
import pathlib
import sys

src = pathlib.Path(sys.argv[1]).read_text()
ini = src.index("INICIO BLOQUE PRUEBA portal_uri_es_de_maquina")
fin = src.index("FIN BLOQUE PRUEBA portal_uri_es_de_maquina")
bloque = src[ini:fin]
# Se corta la cabecera del comentario del marcador y se deja el cuerpo; y por
# detras, el comentario del marcador FIN (que empieza antes de su texto).
bloque = bloque[bloque.index("*/") + 2:]
bloque = bloque[:bloque.rindex("}") + 1]

pruebas = r'''
struct caso { const char *uri; int maquina; const char *por_que; };
int main(void)
{
    static const struct caso casos[] = {
        /* Lo que mandan las cabinas: NO es "alguien mirando". */
        { "/api/viaje",                     1, "apuntes del cuaderno" },
        { "/api/viaje?x=1",                 1, "apuntes con query" },
        { "/api/alarma",                    1, "callar la alarma" },
        { "/api/pantalla/info",             1, "comprobacion de firmware" },
        { "/api/pantalla/bin?modelo=cabina5", 1, "descarga del .bin" },
        /* Lo que mira una persona: SI cuenta. */
        { "/",                              0, "el portal" },
        { "/data",                          0, "indice de datos" },
        { "/data/viajes",                   0, "lista de viajes" },
        { "/api/state",                     0, "la app sondeando a 1 Hz" },
        { "/api/perf",                      0, "rendimiento (app)" },
        { "/keys",                          0, "las claves de los Victron" },
        { "/data/frigo.csv",                0, "descargar un CSV" },
        { "/vigilancia.json",               0, "galeria de la camara" },
        /* Basura: se trata como maquina (no enciende el icono por nada). */
        { "",                               1, "vacio" },
        { NULL,                             1, "sin uri" },
    };
    int fallos = 0;
    for (unsigned i = 0; i < sizeof(casos) / sizeof(casos[0]); i++) {
        int r = portal_uri_es_de_maquina(casos[i].uri) ? 1 : 0;
        if (r != casos[i].maquina) {
            printf("FALLO  %-32s esperado %s, salio %s  (%s)\n",
                   casos[i].uri ? casos[i].uri : "(NULL)",
                   casos[i].maquina ? "maquina" : "cliente",
                   r ? "maquina" : "cliente", casos[i].por_que);
            fallos++;
        }
    }
    if (fallos) { printf("%d fallo(s) en portal_uri_es_de_maquina\n", fallos); return 1; }
    printf("ok: %u casos de portal_uri_es_de_maquina\n",
           (unsigned)(sizeof(casos) / sizeof(casos[0])));
    return 0;
}
'''

cabecera = '''
/* Generado por test/portal_actividad.sh a partir de main/portal/config_server.c */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

'''
pathlib.Path(sys.argv[2]).write_text(cabecera + bloque + pruebas)
PY

gcc -std=gnu11 -Wall -Wextra -o "$TMP/caso" "$TMP/caso.c"
"$TMP/caso"
