#!/usr/bin/env bash
#
# aplicar_parche_idf.sh — aplica al ESP-IDF instalado los parches de patches/.
#
# POR QUE EXISTE (auditoria del 23-sep-2026, punto pendiente): el IDF fijado es
# v5.5.5, que es ANTERIOR al arreglo oficial de Espressif
# 6c1f0e7b59 ("avoid NULL memcpy when private DMA buffer setup fails", entra en
# 5.5.6). Sin ese arreglo, un fallo de RAM DMA al hablar con la SD no devuelve
# error: revienta con "Load access fault" (memcpy desde NULL). Ver
# patches/LEEME.md.
#
# Antes esto vivia SOLO dentro de release.sh, asi que:
#   - los builds locales salian bien (release.sh ya habia parcheado el IDF del
#     sistema, y el parche es persistente), pero
#   - el CI compilaba con el IDF PRISTINO, o sea un firmware distinto del que
#     se publica. Eso es justo lo que no puede ser.
# Ahora la logica esta en un solo sitio y la llaman los tres: release.sh,
# scripts/build_p4.sh y el workflow del CI.
#
# Es IDEMPOTENTE: si el IDF ya lleva el arreglo (o el dia que se suba a 5.5.6+),
# no hace nada y sale con 0.
#
# Uso:  ./scripts/aplicar_parche_idf.sh          (necesita IDF_PATH)
#       IDF_PATH=/ruta/al/idf ./scripts/aplicar_parche_idf.sh
#
set -euo pipefail

AQUI="$(cd "$(dirname "$0")/.." && pwd)"

if [ -z "${IDF_PATH:-}" ]; then
    echo "ERROR: IDF_PATH no esta definido. Fuentea el export.sh del IDF antes." >&2
    exit 1
fi

# TODOS los parches de patches/, de uno en uno y sin listas a mano (05-nov-2026):
# antes solo se aplicaba el del SPI, asi que añadir otro obligaba a tocar este
# script. Cada parche se detecta por si mismo:
#   - si el arbol YA lo lleva (apply --check --reverse pasa), se salta;
#   - si aplica, se aplica;
#   - si ni una cosa ni la otra, es que el IDF ha cambiado: error.
shopt -s nullglob
PARCHES=( "$AQUI"/patches/*.patch )
if [ ${#PARCHES[@]} -eq 0 ]; then
    echo "[i] no hay parches en $AQUI/patches: nada que hacer"
    exit 0
fi

fallo=0
for PARCHE in "${PARCHES[@]}"; do
    nombre="$(basename "$PARCHE")"
    if git -C "$IDF_PATH" apply --check --reverse "$PARCHE" 2>/dev/null; then
        echo "[ok] el IDF ya lleva $nombre"
        continue
    fi
    if git -C "$IDF_PATH" apply --check "$PARCHE" 2>/dev/null; then
        git -C "$IDF_PATH" apply "$PARCHE"
        echo "[ok] parche aplicado al IDF: $nombre"
        continue
    fi
    echo "ERROR: $nombre no aplica al IDF (ni esta ya puesto)." >&2
    fallo=1
done

if [ "$fallo" -ne 0 ]; then
    echo "       IDF_PATH=$IDF_PATH" >&2
    echo "       Revisa patches/ y el estado del arbol del IDF (git -C \"\$IDF_PATH\" status)." >&2
    exit 1
fi
exit 0
