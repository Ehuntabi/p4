/* perf.c — numeros de rendimiento de la P4 (ver perf.h).
 *
 * TODO LO QUE HACE ES LEER CONTADORES. No cambia el comportamiento del aparato:
 *  - la muestra de CPU es una tarea de esp_timer cada 10 s (microsegundos);
 *  - el flush de LVGL solo se cronometra (se llama al original igual);
 *  - el datalogger avisa de cuanto tarda su volcado a la SD;
 *  - cada 2 ms se mira el nivel del FIFO del puente DSI (ver seccion 3 bis).
 *
 * COMO SE LEE EL % DE CPU: el contador de FreeRTOS es tiempo de CPU de la tarea
 * (con CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER va en microsegundos). El
 * porcentaje es contra el tiempo de RELOJ transcurrido, asi que una tarea que
 * se come un nucleo entero da 100 %; la P4 tiene DOS nucleos, asi que la suma de
 * todos los porcentajes puede pasar de 100. Es la misma convencion que "top".
 */
#include "perf.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "datalogger.h"
#include "lvgl.h"
#include "soc/mipi_dsi_bridge_reg.h"
#include "display.h"           /* brillo y LEDC de la retroiluminacion */
#include "driver/ledc.h"

static const char *TAG = "perf";

static void perf_mira_sd(void);   /* se define mas abajo */

/* ── 1. CPU y pila por tarea ─────────────────────────────────────────────── */
#define PERF_MAX_TAREAS 40

typedef struct {
    TaskHandle_t h;
    char         nombre[configMAX_TASK_NAME_LEN];
    uint32_t     tics;              /* ulRunTimeCounter de la muestra anterior */
} perf_previa_t;

static perf_previa_t s_previa[PERF_MAX_TAREAS];
static uint32_t      s_previa_n;
static int64_t       s_previa_us;

typedef struct {
    char     nombre[configMAX_TASK_NAME_LEN];
    uint32_t cpu_pct;
    uint32_t pila_libre;
    int      nucleo;        /* 0/1, o -1 si la tarea no esta fijada */
} perf_uso_t;

static perf_uso_t s_uso[PERF_MAX_TAREAS];
static uint32_t   s_uso_n;

static void perf_muestra(void)
{
    perf_mira_sd();
    static TaskStatus_t st[PERF_MAX_TAREAS];
    uint32_t n = uxTaskGetSystemState(st, PERF_MAX_TAREAS, NULL);
    int64_t ahora = esp_timer_get_time();
    int64_t dt_us = s_previa_us ? (ahora - s_previa_us) : 0;

    s_uso_n = 0;
    for (uint32_t i = 0; i < n && i < PERF_MAX_TAREAS; i++) {
        uint32_t antes = 0;
        for (uint32_t j = 0; j < s_previa_n; j++) {
            if (s_previa[j].h == st[i].xHandle) { antes = s_previa[j].tics; break; }
        }
        uint32_t delta = st[i].ulRunTimeCounter - antes;   /* admite el wrap */
        uint32_t pct = 0;
        if (dt_us > 0) {
            int64_t p = (int64_t)delta * 100 / dt_us;
            pct = (p < 0) ? 0 : (p > 100) ? 100 : (uint32_t)p;
        }
        strncpy(s_uso[i].nombre, st[i].pcTaskName, sizeof(s_uso[i].nombre) - 1);
        s_uso[i].nombre[sizeof(s_uso[i].nombre) - 1] = '\0';
        s_uso[i].cpu_pct = pct;
        s_uso[i].pila_libre = (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t);
        {
            BaseType_t af = xTaskGetAffinity(st[i].xHandle);
            s_uso[i].nucleo = (af == tskNO_AFFINITY) ? -1 : (int)af;
        }
    }
    s_uso_n = (n < PERF_MAX_TAREAS) ? n : PERF_MAX_TAREAS;

    for (uint32_t i = 0; i < s_uso_n; i++) {
        s_previa[i].h = st[i].xHandle;
        s_previa[i].tics = st[i].ulRunTimeCounter;
        strncpy(s_previa[i].nombre, st[i].pcTaskName, sizeof(s_previa[i].nombre) - 1);
        s_previa[i].nombre[sizeof(s_previa[i].nombre) - 1] = '\0';
    }
    s_previa_n = s_uso_n;
    s_previa_us = ahora;
}

/* ── 2. Volcado a la SD (datalogger) ─────────────────────────────────────── */
static uint32_t s_sd_n;
static uint32_t s_sd_fallos;
static int64_t  s_sd_suma_us;
static int64_t  s_sd_max_us;
static uint32_t s_sd_vistos;

/* El datalogger lleva sus propios contadores (component/datalogger): aqui solo
 * se recogen los nuevos en cada muestra. Asi el componente no depende de este
 * fichero, que vive en main/. */
static void perf_mira_sd(void)
{
    uint32_t n = datalogger_volcados();
    if (n != s_sd_vistos) {
        s_sd_n += (n - s_sd_vistos);
        s_sd_suma_us += datalogger_ultimo_volcado_us();
        if (datalogger_max_volcado_us() > s_sd_max_us) s_sd_max_us = datalogger_max_volcado_us();
        s_sd_vistos = n;
    }
    s_sd_fallos = datalogger_volcado_fallos();
}

/* ── 3. Flush de LVGL ────────────────────────────────────────────────────── */
static void (*s_flush_original)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static uint32_t s_flush_n;
static int64_t  s_flush_suma_us;
static int64_t  s_flush_max_us;

static void perf_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    int64_t t0 = esp_timer_get_time();
    if (s_flush_original) s_flush_original(drv, area, px);
    int64_t dt = esp_timer_get_time() - t0;
    s_flush_n++;
    s_flush_suma_us += dt;
    if (dt > s_flush_max_us) s_flush_max_us = dt;
}

void perf_envuelve_flush(void *disp_v)
{
    lv_disp_t *disp = (lv_disp_t *)disp_v;
    if (!disp || !disp->driver || !disp->driver->flush_cb) {
        ESP_LOGW(TAG, "sin display: no se mide el volcado de LVGL");
        return;
    }
    s_flush_original = disp->driver->flush_cb;
    disp->driver->flush_cb = perf_flush_cb;
}

/* ── 3 bis. Underruns del puente DSI (NO eran el flash de la P4) ──────────
 *
 * QUE ES UN UNDERRUN. Si al puente DSI le faltan datos a tiempo, en vez de la
 * imagen manda al panel su valor de reserva (`dpi_rsv_data`, 0x3FFF: un color
 * muy claro, ver mipi_dsi_bridge_struct.h) y el panel se ve de ese color
 * mientras dura, sin imagen. Es lo que documenta IDF en esp_lcd_panel_dpi.c
 * ("can't fetch data from external memory fast enough, underrun happens") y el
 * caso P4+MIPI-DSI de github.com/jantielens/esp32-macropad/issues/7.
 *
 * POR QUE SE MIDE AQUI. El 09-nov-2026 el usuario describio "un flash claro" en
 * la pantalla y esto era el primer sospechoso (ya habia pasado en las OTA, ver
 * el commit del 06-oct-2026). Se cuenta leyendo el bit raw cada 2 ms.
 *
 * RESULTADO (10-nov-2026): descartado. Con la pantalla dando flashes se
 * contaron CERO underruns, tanto en reposo como bajando 6 MB por Wi-Fi con la
 * pantalla encendida. El flash de la P4 es la luz de fondo (ver 3 ter), no los
 * datos: por eso la imagen se sigue viendo.
 *
 * HAY QUE APAGAR LA INTERRUPCION DEL DRIVER. IDF habilita la interrupcion de
 * underrun (esp_lcd_panel_dpi.c: `mipi_dsi_brg_ll_enable_interrupt(...,
 * MIPI_DSI_BRG_LL_EVENT_UNDERRUN, true)`) y su ISR BORRA el bit en
 * microsegundos, asi que desde una tarea se lee siempre 0 y no se entera de
 * nada (comprobado: el contador daba cero con la pantalla parpadeando). Se
 * deshabilita esa interrupcion y se borra lo pendiente: el bit se queda
 * latcheado y lo cuenta esta tarea, que es la unica que lo mira. El driver solo
 * usaba ese aviso para un ESP_DRAM_LOGE (que el log capturado ni siquiera
 * recogia); ahora lo da perf, con contador, brillo y fecha.
 *
 * NOTA: el registro `fifo_flow_status` NO sirve para medir el nivel del FIFO --
 * devuelve 768 fijo (el umbral "casi vacio" que configura el driver).
 */
static uint32_t s_dsi_underruns;
static int64_t  s_dsi_ultimo_aviso_us;

/* Se llama UNA vez, con la pantalla ya creada (perf_start) y antes de arrancar
 * el temporizador de 2 ms. */
static void perf_dsi_quita_isr(void)
{
    REG_WRITE(DSI_BRG_INT_ENA_REG, REG_READ(DSI_BRG_INT_ENA_REG) & ~DSI_BRG_UNDERRUN_INT_ENA);
    REG_WRITE(DSI_BRG_INT_CLR_REG, DSI_BRG_UNDERRUN_INT_CLR);
}

static void perf_mira_dsi(void)
{
    if (!(REG_READ(DSI_BRG_INT_RAW_REG) & DSI_BRG_UNDERRUN_INT_RAW)) return;

    REG_WRITE(DSI_BRG_INT_CLR_REG, DSI_BRG_UNDERRUN_INT_CLR);
    s_dsi_underruns++;

    /* Un aviso al log como maximo cada 2 s (si no, el propio log se come el
     * bus que le falta a la pantalla). El contador va en el aviso. */
    int64_t ahora = esp_timer_get_time();
    if (s_dsi_ultimo_aviso_us == 0 || ahora - s_dsi_ultimo_aviso_us > 2000000) {
        s_dsi_ultimo_aviso_us = ahora;
        ESP_LOGW(TAG, "DSI: underrun (la pantalla se pone clara un instante); van %u, brillo %d%%",
                 (unsigned)s_dsi_underruns, bsp_display_brightness_get());
    }
}

/* ── 3 ter. Duty de la retroiluminacion: DONDE ESTA el flash de la P4 ────
 *
 * El 10-nov-2026 el usuario afino el sintoma: en el flash SE SIGUE VIENDO LA
 * IMAGEN, solo que todo mas claro. Eso descarta el DSI (que se comeria la
 * imagen, ver 3 bis) y senala la LUZ DE FONDO.
 *
 * Aqui se vigila el duty del LEDC. Si el flash coincidiera con un cambio de
 * duty, lo estaria pidiendo el software (y habria bug que arreglar). Medido el
 * 10-nov-2026, con el usuario viendo flashes: el duty solo cambio 3 veces en 6
 * minutos, y las tres fueron las esperadas (80 % del arranque, el 50 % de
 * Ajustes y la atenuacion del salvapantallas al 20 %). Cero cambios durante los
 * flashes -> el salto es ELECTRICO (alimentacion del banco o driver del panel),
 * no del firmware. Con 12 V (la autocaravana) no pasaba y con los 5 V del banco
 * si; ademas la placa se reinicio una vez con motivo `poweron` estando en el
 * cargador. Se deja la medida puesta porque es la que lo demuestra y porque
 * cualquier cambio de brillo futuro queda fechado en el log.
 */
static uint32_t s_duty_visto;
static uint32_t s_duty_cambios;

static void perf_mira_duty(void)
{
    uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL_NUM);
    if (duty == s_duty_visto) return;

    s_duty_cambios++;
    ESP_LOGW(TAG, "LEDC backlight: duty %u -> %u (%d%% pedido)",
             (unsigned)s_duty_visto, (unsigned)duty, bsp_display_brightness_get());
    s_duty_visto = duty;
}

/* ── 4. Heap, reinicios y version ────────────────────────────────────────── */
static const char *motivo_reinicio(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_JTAG:     return "jtag";
    case ESP_RST_EFUSE:    return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:               return "desconocido";
    }
}

/* ── 5. JSON y log ───────────────────────────────────────────────────────── */

/* Copia ordenada por CPU (mayor primero). Con 40 tareas y un array pequeno:
 * seleccion directa, es una sola vez por peticion. */
static void perf_ordenadas(perf_uso_t *dst, uint32_t *n)
{
    static bool usada[PERF_MAX_TAREAS];
    memset(usada, 0, sizeof usada);
    uint32_t k = 0;
    for (uint32_t i = 0; i < s_uso_n; i++) {
        uint32_t mejor = PERF_MAX_TAREAS;
        for (uint32_t j = 0; j < s_uso_n; j++) {
            if (usada[j]) continue;
            if (mejor == PERF_MAX_TAREAS || s_uso[j].cpu_pct > s_uso[mejor].cpu_pct) mejor = j;
        }
        if (mejor == PERF_MAX_TAREAS) break;
        usada[mejor] = true;
        dst[k++] = s_uso[mejor];
    }
    *n = k;
}

size_t perf_json(char *out, size_t n)
{
    if (!out || n < 64) return 0;

    /* NO se vuelve a muestrear en cada peticion: la muestra de 10 s ya esta
     * fresca y re-muestrear dos veces seguidas da porcentajes sobre una ventana
     * de milisegundos (basura). Solo se muestrea si aun no hay ninguna. */
    if (s_previa_us == 0) perf_muestra();

    perf_uso_t orden[PERF_MAX_TAREAS];
    uint32_t n_orden = 0;
    perf_ordenadas(orden, &n_orden);

    int64_t ahora = esp_timer_get_time();
    size_t heap_libre  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_mayor  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t heap_minimo = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    size_t w = 0;
    w += (size_t)snprintf(out + w, (w < n) ? n - w : 0,
        "{\"ver\":\"%s\",\"uptime_s\":%lld,\"reinicio\":\"%s\","
        "\"heap\":{\"libre\":%u,\"mayor\":%u,\"minimo\":%u},"
        "\"sd\":{\"n\":%u,\"fallos\":%u,\"media_us\":%lld,\"max_us\":%lld},"
        "\"lvgl\":{\"bloques\":%u,\"media_us\":%lld,\"max_us\":%lld},"
        "\"dsi\":{\"underruns\":%u,\"isr\":%u},"
        "\"luz\":{\"duty\":%u,\"cambios\":%u},"

        "\"tareas\":[",
        esp_app_get_description()->version,
        (long long)(ahora / 1000000),
        motivo_reinicio(esp_reset_reason()),
        (unsigned)heap_libre, (unsigned)heap_mayor, (unsigned)heap_minimo,
        (unsigned)s_sd_n, (unsigned)s_sd_fallos,
        (long long)(s_sd_n ? s_sd_suma_us / s_sd_n : 0), (long long)s_sd_max_us,
        (unsigned)s_flush_n,
        (long long)(s_flush_n ? s_flush_suma_us / s_flush_n : 0), (long long)s_flush_max_us,
        (unsigned)s_dsi_underruns,
        (unsigned)((REG_READ(DSI_BRG_INT_ENA_REG) & DSI_BRG_UNDERRUN_INT_ENA) ? 1 : 0),
        (unsigned)s_duty_visto, (unsigned)s_duty_cambios);


    for (uint32_t i = 0; i < n_orden && w + 64 < n; i++) {
        w += (size_t)snprintf(out + w, n - w,
            "%s{\"n\":\"%s\",\"cpu\":%u,\"pila\":%u,\"nucleo\":%d}",
            i ? "," : "", orden[i].nombre, (unsigned)orden[i].cpu_pct,
            (unsigned)orden[i].pila_libre, orden[i].nucleo);
    }
    w += (size_t)snprintf(out + w, (w < n) ? n - w : 0, "]}");
    return w;
}

void perf_log(void)
{
    perf_muestra();
    perf_uso_t orden[PERF_MAX_TAREAS];
    uint32_t n_orden = 0;
    perf_ordenadas(orden, &n_orden);

    char top[96] = "";
    size_t w = 0;
    for (uint32_t i = 0; i < n_orden && i < 4 && w < sizeof top; i++) {
        w += (size_t)snprintf(top + w, sizeof top - w, "%s%s=%u%%",
                              i ? " " : "", orden[i].nombre, (unsigned)orden[i].cpu_pct);
    }
    ESP_LOGI(TAG, "cpu: %s | heap libre=%u mayor=%u | sd media=%lldms max=%lldms (n=%u) | lvgl media=%lldus max=%lldus | dsi underrun=%u (isr=%u) | luz duty=%u (%u cambios)",
             top,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (long long)(s_sd_n ? s_sd_suma_us / s_sd_n / 1000 : 0),
             (long long)(s_sd_max_us / 1000),
             (unsigned)s_sd_n,
             (long long)(s_flush_n ? s_flush_suma_us / s_flush_n : 0),
             (long long)s_flush_max_us,
             (unsigned)s_dsi_underruns,
        (unsigned)((REG_READ(DSI_BRG_INT_ENA_REG) & DSI_BRG_UNDERRUN_INT_ENA) ? 1 : 0),
        (unsigned)s_duty_visto, (unsigned)s_duty_cambios);
}

/* ── 6. Arranque ─────────────────────────────────────────────────────────── */
static void perf_tick(void *arg)
{
    (void)arg;
    static uint32_t tics;
    perf_muestra();
    /* Un resumen al log cada 10 min (60 muestras de 10 s), para tener rastro
     * de una fuga o de un pico sin tener que pedir /api/perf. */
    if (++tics % 60 == 0) perf_log();
}

/* Muestra del FIFO del DSI: 2 ms basta para no perder una hambruna de varios
 * milisegundos (un frame son 16,6 ms) y cuesta ~0,25 % de CPU. */
static void perf_tick_rapido(void *arg)
{
    (void)arg;
    perf_mira_dsi();
    perf_mira_duty();
}

void perf_start(void)
{
    static esp_timer_handle_t t;
    if (t) return;
    const esp_timer_create_args_t args = {
        .callback = perf_tick,
        .name = "perf",
    };
    if (esp_timer_create(&args, &t) != ESP_OK) {
        ESP_LOGW(TAG, "no pude crear el temporizador de medidas");
        return;
    }
    esp_timer_start_periodic(t, 10 * 1000 * 1000);

    perf_dsi_quita_isr();

    static esp_timer_handle_t t_dsi;
    const esp_timer_create_args_t args_dsi = {
        .callback = perf_tick_rapido,
        .name = "perf_dsi",
    };
    if (esp_timer_create(&args_dsi, &t_dsi) != ESP_OK) {
        ESP_LOGW(TAG, "no pude crear el temporizador del DSI");
    } else {
        esp_timer_start_periodic(t_dsi, 2 * 1000);
    }

    perf_muestra();   /* primera referencia para el primer % */
    perf_mira_dsi();
    s_duty_visto = ledc_get_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL_NUM);
    ESP_LOGI(TAG, "medidas activas (CPU por tarea, heap, SD, LVGL, FIFO del DSI); /api/perf");
}
