# Genera version_build.h: el SELLO de lo que se esta compilando.
#
# POR QUE EXISTE (22-nov-2026; el dueno: "si veo 14 NOV pienso que no esta en la
# ultima version"). La pantalla ensenaba app->date/app->time del descriptor de
# ESP-IDF, que se rellena al compilar ESE componente y NO se regenera si no
# cambia: se cambio codigo el 22, se compilo y se grabo el 22, y seguia diciendo
# 14-nov. Un rotulo que miente es peor que no tenerlo.
#
# Aqui la FECHA es la del COMMIT (lo que de verdad hay dentro), no la de la
# compilacion, y el TAG sale de git describe: con el tag exacto si esta
# publicado, o "v1.89-4-g63d8076" si lleva commits por encima. Los cambios sin
# commitear se marcan APARTE (JB_BUILD_DIRTY): el numero del tag se queda limpio,
# que es el que va en el nombre del .bin.
#
# Uso:
#   cmake -DREPO=<repo> -DOUT=<ruta/version_build.h> [-DSTAMP_FILE=<fichero>] \
#         -P scripts/gen_version_header.cmake
#
# STAMP_FILE es para los builds en COPIA SIN .git (scripts/build_p4.sh): cuatro
# lineas con describe, commit, fecha (%d-%b-%Y_%H:%M) y 0/1 de sucio, escritas
# por el script desde el arbol de verdad. Sin eso, una copia sin .git solo podria
# decir "sin-git" y volveriamos al "v?" que arreglo version.txt.

if(NOT REPO OR NOT OUT)
    message(FATAL_ERROR "gen_version_header: faltan -DREPO o -DOUT")
endif()

set(TAG "")
set(COMMIT "")
set(FECHA "")
set(DIRTY 0)

# ¿REPO es la raiz de SU repo? Sin esta comprobacion, un arbol copiado sin .git
# (el espejo de build_p4.sh) heredaria el repo que tenga por encima y el sello
# diria el commit de OTRO arbol.
execute_process(
    COMMAND git -C "${REPO}" rev-parse --show-toplevel
    OUTPUT_VARIABLE RAIZ
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RC)
if(GIT_RC EQUAL 0 AND RAIZ STREQUAL REPO)
    execute_process(
        COMMAND git -C "${REPO}" describe --tags --always
        OUTPUT_VARIABLE TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${REPO}" rev-parse --short HEAD
        OUTPUT_VARIABLE COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${REPO}" log -1 --format=%cd --date=format:%d-%b-%Y_%H:%M
        OUTPUT_VARIABLE FECHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # Sucio = cambios en ficheros VERSIONADOS. dependencies.lock se queda fuera
    # porque el propio build lo reescribe con rutas de esta maquina: marcarlo
    # pondria "sin commitear" en todos los builds aunque el codigo sea el del
    # commit (la misma exclusion que scripts/bin_con_version.sh).
    execute_process(
        COMMAND git -C "${REPO}" status --porcelain --untracked-files=no --
                . ":(exclude)dependencies.lock"
        OUTPUT_VARIABLE SUCIO
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(SUCIO)
        set(DIRTY 1)
    endif()
elseif(STAMP_FILE AND EXISTS "${STAMP_FILE}")
    # Copia sin .git: el sello lo dejo escrito quien preparo la copia.
    file(STRINGS "${STAMP_FILE}" LINEAS ENCODING UTF-8)
    list(LENGTH LINEAS N_LINEAS)
    if(N_LINEAS GREATER_EQUAL 4)
        list(GET LINEAS 0 TAG)
        list(GET LINEAS 1 COMMIT)
        list(GET LINEAS 2 FECHA)
        list(GET LINEAS 3 DIRTY)
        string(STRIP "${TAG}" TAG)
        string(STRIP "${COMMIT}" COMMIT)
        string(STRIP "${FECHA}" FECHA)
        string(STRIP "${DIRTY}" DIRTY)
        if(NOT DIRTY MATCHES "^[01]$")
            set(DIRTY 0)
        endif()
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
else()
    message(STATUS "sello de version: ${TAG} (${COMMIT}) ${FECHA}")
endif()
