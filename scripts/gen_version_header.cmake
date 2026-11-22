# Genera version_build.h: el SELLO de lo que se esta compilando.
#
# POR QUE EXISTE (22-nov-2026; el dueno: "si veo 14 NOV pienso que no esta en la
# ultima version"). La pantalla ensenaba app->date/app->time del descriptor de
# ESP-IDF, que se rellena al compilar ESE componente y NO se regenera si no
# cambia: con codigo del 22 dentro seguia diciendo 14-nov. Un rotulo que miente
# es peor que no tenerlo.
#
# Aqui la FECHA es la del COMMIT (lo que de verdad hay dentro), no la de la
# compilacion. El TAG sale de git describe FILTRADO a versiones (v*.*), igual que
# PROJECT_VER, para que un tag de otra cosa (sdkconfig-known-good-*, que la P4
# tiene desde julio) no se cuele como "version". Y una FUENTE NUEVA sin "git add"
# tambien cuenta como "sin commitear": los GLOB de main/CMakeLists la meten en el
# build, asi que el .bin puede llevar codigo que no esta en ningun commit.
#
# Uso:
#   cmake -DREPO=<repo> -DOUT=<ruta/version_build.h> [-DSTAMP_FILE=<fichero>] \
#         -P scripts/gen_version_header.cmake
#
# STAMP_FILE es para los builds en COPIA SIN .git (scripts/build_p4.sh): cuatro
# lineas con describe, commit, fecha (%d-%b-%Y_%H:%M) y 0/1 de sucio, escritas
# por el script desde el arbol de verdad. Sin eso, una copia sin .git solo podria
# decir "sin-git" y volveriamos al "v?" que arreglo version.txt.
#
# OJO CON CUANDO CORRE (auditoria del 22-nov-2026): esto se ejecuta al CONFIGURAR.
# CMake vuelve a configurar solo si cambia algo de lo que vigila el CMakeLists
# (.git/HEAD, .git/index, .git/refs/heads, .git/refs/tags, .git/packed-refs y
# este script). Editar un fuente y compilar SIN reconfigure deja el sello como
# estaba; los scripts de grabar/publicar exigen arbol limpio y reconfigure, y
# release.sh comprueba ademas que el sello sea el del tag. No se puede generar en
# tiempo de BUILD: el compilador puede arrancar antes de que exista la cabecera
# (probado con CMake+Ninja el 22-nov), y un build desde cero fallaria.

if(NOT REPO OR NOT OUT)
    message(FATAL_ERROR "gen_version_header: faltan -DREPO o -DOUT")
endif()

set(TAG "")
set(COMMIT "")
set(FECHA "")
set(DIRTY 0)

# ¿REPO es la raiz de SU repo? Se pregunta por --show-prefix (vacio = raiz) y no
# comparando con --show-toplevel: por una ruta con enlace simbolico git devuelve
# la ruta fisica y el sello caia a "sin-git" en silencio (auditoria del 22-nov).
execute_process(
    COMMAND git -C "${REPO}" rev-parse --show-prefix
    OUTPUT_VARIABLE PREFIJO
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RC)

if(GIT_RC EQUAL 0 AND PREFIJO STREQUAL "")
    execute_process(
        COMMAND git -C "${REPO}" describe --tags --always --match "v*.*"
        OUTPUT_VARIABLE TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${REPO}" rev-parse --short HEAD
        OUTPUT_VARIABLE COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # LC_ALL=C: el nombre del mes de %b depende del locale y la tabla de abajo
    # solo traduce del INGLES. Sin esto, en un equipo en frances el rotulo salia
    # en frances (auditoria del 22-nov-2026).
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
                git -C "${REPO}" log -1 --format=%cd --date=format:%d-%b-%Y_%H:%M
        OUTPUT_VARIABLE FECHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # Sucio = cambios en ficheros VERSIONADOS, MAS fuentes nuevas sin "git add".
    # dependencies.lock se queda fuera de los versionados porque el propio build
    # lo reescribe con rutas de esta maquina (misma exclusion que
    # scripts/bin_con_version.sh): marcarlo pondria "sin commitear" en todos los
    # builds aunque el codigo sea el del commit.
    execute_process(
        COMMAND git -C "${REPO}" status --porcelain --untracked-files=no --
                . ":(exclude)dependencies.lock"
        OUTPUT_VARIABLE TOCADOS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${REPO}" status --porcelain --untracked-files=all --
                "*.c" "*.h"
        OUTPUT_VARIABLE SUELTOS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(TOCADOS OR SUELTOS)
        set(DIRTY 1)
    endif()
elseif(STAMP_FILE AND EXISTS "${STAMP_FILE}")
    # Copia sin .git: el sello lo dejo escrito quien preparo la copia.
    file(STRINGS "${STAMP_FILE}" LINEAS ENCODING UTF-8)
    list(LENGTH LINEAS N_LINEAS)
    if(N_LINEAS LESS 4)
        message(FATAL_ERROR "el sello de la copia (${STAMP_FILE}) tiene ${N_LINEAS} lineas y hacen falta 4: tag, commit, fecha y 0/1 de sucio")
    endif()
    list(GET LINEAS 0 TAG)
    list(GET LINEAS 1 COMMIT)
    list(GET LINEAS 2 FECHA)
    list(GET LINEAS 3 DIRTY)
    string(STRIP "${TAG}" TAG)
    string(STRIP "${COMMIT}" COMMIT)
    string(STRIP "${FECHA}" FECHA)
    string(STRIP "${DIRTY}" DIRTY)
    if(NOT DIRTY MATCHES "^[01]$")
        message(FATAL_ERROR "el sello de la copia (${STAMP_FILE}) trae '${DIRTY}' donde va 0 o 1 (sucio)")
    endif()
    if(NOT FECHA MATCHES "^([0-9]+-[A-Za-z]+-[0-9]+[ _][0-9]+:[0-9]+|sin-fecha)$")
        message(FATAL_ERROR "el sello de la copia (${STAMP_FILE}) trae una fecha rara: '${FECHA}'")
    endif()
endif()

if(NOT TAG)
    set(TAG "sin-git")
endif()
if(NOT COMMIT)
    set(COMMIT "sin-git")
endif()
if(NOT FECHA)
    set(FECHA "sin-fecha")
endif()

string(REPLACE "_" " " FECHA "${FECHA}")
# El mes de git sale en ingles (22-Nov-2026): en la pantalla se lee en espanol.
set(MESES_EN Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
set(MESES_ES ene feb mar abr may jun jul ago sep oct nov dic)
list(LENGTH MESES_EN N_MESES)
math(EXPR ULTIMO_MES "${N_MESES} - 1")
foreach(i RANGE ${ULTIMO_MES})
    list(GET MESES_EN ${i} MES_EN)
    list(GET MESES_ES ${i} MES_ES)
    string(REPLACE "${MES_EN}" "${MES_ES}" FECHA "${FECHA}")
endforeach()

set(CONTENIDO
"/* GENERADO por scripts/gen_version_header.cmake. No editar a mano.
 *
 * El sello de lo que va DENTRO de este firmware (22-nov-2026): tag y commit de
 * git, y la fecha DEL COMMIT (no la de la compilacion, que se queda vieja y
 * miente: el 22-nov la placa decia 14-nov).
 */
#pragma once
#define JB_VERSION_TAG  \"${TAG}\"
#define JB_COMMIT       \"${COMMIT}\"
#define JB_BUILD_STAMP  \"${FECHA}\"
#define JB_BUILD_DIRTY  ${DIRTY}
")

# Escribir SOLO si cambia: el configure corre muchas veces (git status toca el
# indice, se reconfigura...) y reescribir el fichero con lo mismo obligaria a
# recompilar la pantalla entera sin motivo.
if(EXISTS "${OUT}")
    file(READ "${OUT}" ANTERIOR)
    if(ANTERIOR STREQUAL CONTENIDO)
        message(STATUS "sello de version: ${TAG} (${COMMIT}) ${FECHA} -- sin cambios")
        return()
    endif()
endif()
file(WRITE "${OUT}" "${CONTENIDO}")
if(DIRTY)
    message(STATUS "sello de version: ${TAG} (${COMMIT}) ${FECHA} -- CON CAMBIOS SIN COMMITEAR")
elseif(TAG STREQUAL "sin-git")
    message(STATUS "sello de version: SIN DATOS DE GIT (${FECHA})")
else()
    message(STATUS "sello de version: ${TAG} (${COMMIT}) ${FECHA}")
endif()
