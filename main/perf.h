/* perf.h — numeros de rendimiento de la P4, para /api/perf y el log periodico.
 *
 * POR QUE EXISTE (09-nov-2026). Hasta hoy no habia ni un dato medido: "va bien"
 * era una impresion. Esto recoge, sin coste apreciable:
 *   - % de CPU por tarea (necesita CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
 *   - pila minima libre por tarea (lo que ya miraba stack_watch, pero en JSON)
 *   - heap interno: libre, bloque mas grande y minimo historico (fragmentacion)
 *   - coste del volcado a la SD (media y maximo del fopen..fclose del datalogger)
 *   - coste del volcado de LVGL por bloque (media y maximo del flush_cb)
 *   - tiempo en marcha, motivo del ultimo reinicio y version
 *
 * Los contadores de CPU se calculan por DIFERENCIA entre muestras: la primera
 * muestra (o una peticion antes de los primeros 10 s) sale a cero a proposito.
 */
#ifndef PERF_H
#define PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Envuelve el flush_cb de LVGL para medir el coste de cada bloque. Llamar UNA
 * vez, despues de crear la pantalla (bsp_display_start). */
void perf_envuelve_flush(void *disp);

/* Arranca las medidas: los contadores de SD/LVGL, el resumen al log cada 10
 * min (CPU global por contadores de ralenti, heap, SD y LVGL) y el envoltorio
 * del flush. NO arranca ninguna muestra periodica de tareas: uxTaskGetSystemState
 * solo se llama desde GET /api/perf (era la causa del destello de la pantalla). */
void perf_start(void);

/* Construye el JSON de /api/perf en 'out'. Devuelve los bytes escritos. */
size_t perf_json(char *out, size_t n);

/* Una linea al log con el resumen, sin tocar el planificador. La llama el tic
 * de 10 s cada 60 vueltas (cada 10 min). */
void perf_log(void);

#ifdef __cplusplus
}
#endif

#endif /* PERF_H */
