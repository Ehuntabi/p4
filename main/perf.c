/* perf.c — numeros de rendimiento de la P4 (ver perf.h).
 *
 * TODO LO QUE HACE ES LEER CONTADORES. No cambia el comportamiento del aparato:
 *  - la muestra de CPU es una tarea de esp_timer cada 10 s (microsegundos);
 *  - el flush de LVGL solo se cronometra (se llama al original igual);
 *  - el datalogger avisa de cuanto tarda su volcado a la SD.
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
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "datalogger.h"
#include "lvgl.h"
#include "soc/mipi_dsi_bridge_reg.h"
#include "display.h"   /* brillo y canal del LEDC: underrun y sonda de la luz */
#include "esp_bsp.h"    /* bsp_display_lock/unlock para el control positivo A */

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

/* ── 3 bis. El "flaseo" de la pantalla (11-nov-2026) ───────────────────────
 *
 * AQUI HUBO UN FALLO DE MEDIDA, y conviene dejarlo escrito: la primera version
 * solo miraba los volcados de 1024x600 enteros. Este BSP NO hace eso nunca --
 * el buffer de dibujo son 50 lineas (BSP_LCD_DRAW_BUFF_SIZE, display.h), asi
 * que LVGL manda bandas de 1024x50 y la condicion `w>=1000 && h>=580` no se
 * cumplia jamas. El detector de "volcado claro" estuvo 40 minutos sin medir
 * nada y aun asi se dio por descartada esa rama.
 *
 * Ahora se mide BANDA A BANDA (la media de cada volcado, del tamano que sea) y
 * se agrega la media de la pantalla completa con lv_disp_flush_is_last(). Sale:
 *  - BANDA CLARA: una banda suelta clarisima (media > 220). Con una UI de fondo
 *    0x0A0D13 (casi negro), eso es un blanco o un gris muy claro de verdad.
 *  - PANTALLA: la media de la pantalla entera cuando LVGL la repinta (y aviso
 *    si sube de golpe).
 *  - REGION: una actualizacion parcial grande, con su media.
 */
static uint32_t s_pant_suma, s_pant_pix;      /* acumulado de la pantalla en curso */
static uint16_t s_pant_media_previa;
static uint32_t s_pant_completas, s_bandas_claras, s_regiones;
static int64_t  s_banda_aviso_us, s_region_aviso_us;

static uint16_t media_de(const lv_area_t *area, const lv_color_t *px)
{
    uint32_t n = (uint32_t)lv_area_get_width((lv_area_t *)area)
               * (uint32_t)lv_area_get_height((lv_area_t *)area);
    if (n == 0) return 0;
    uint32_t suma = 0, cuenta = 0;
    for (uint32_t i = 0; i < n; i += 64) {
        uint32_t c = lv_color_to32(px[i]);
        suma += ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
        cuenta++;
    }
    return (uint16_t)(cuenta ? suma / (3 * cuenta) : 0);
}

static void perf_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    int64_t t0 = esp_timer_get_time();
    if (s_flush_original) s_flush_original(drv, area, px);
    int64_t dt = esp_timer_get_time() - t0;
    s_flush_n++;
    s_flush_suma_us += dt;
    if (dt > s_flush_max_us) s_flush_max_us = dt;

    int w = (int)lv_area_get_width((lv_area_t *)area);
    int h = (int)lv_area_get_height((lv_area_t *)area);
    uint32_t npx = (uint32_t)w * (uint32_t)h;
    if (npx == 0) return;
    uint16_t media = media_de(area, px);

    /* Una banda suelta clarisima: como mucho un aviso cada 200 ms. */
    if (media > 220) {
        s_bandas_claras++;
        int64_t ahora = esp_timer_get_time();
        if (ahora - s_banda_aviso_us > 200000) {
            s_banda_aviso_us = ahora;
            ESP_LOGW(TAG, "BANDA CLARA: media %u en x%d..%d y%d..%d (brillo %d%%, van %u)",
                     media, (int)area->x1, (int)area->x2, (int)area->y1, (int)area->y2,
                     bsp_display_brightness_get(), (unsigned)s_bandas_claras);
        }
    }

    s_pant_suma += (uint32_t)media * npx;
    s_pant_pix  += npx;

    if (!lv_disp_flush_is_last(drv)) return;

    /* Fin de pantalla: decidir si lo pintado es la pantalla entera o un trozo. */
    uint32_t total = (uint32_t)drv->hor_res * (uint32_t)drv->ver_res;
    if (s_pant_pix == 0) return;
    uint16_t media_pant = (uint16_t)(s_pant_suma / s_pant_pix);
    if (total && s_pant_pix >= total * 95 / 100) {
        s_pant_completas++;
        if (media_pant > (uint16_t)(s_pant_media_previa + 25)) {
            ESP_LOGW(TAG, "PANTALLA: repintado CLARO (media %u, antes %u); "
                     "van %u pantallas, %u bandas claras",
                     media_pant, s_pant_media_previa, (unsigned)s_pant_completas,
                     (unsigned)s_bandas_claras);
        }
        s_pant_media_previa = media_pant;
    } else if (total && s_pant_pix > total / 4) {
        /* Region grande (una ventana, un modal, media pantalla). */
        s_regiones++;
        int64_t ahora = esp_timer_get_time();
        if (ahora - s_region_aviso_us > 500000) {
            s_region_aviso_us = ahora;
            ESP_LOGI(TAG, "REGION: %u px pintados, media %u (brillo %d%%)",
                     (unsigned)s_pant_pix, media_pant, bsp_display_brightness_get());
        }
    }
    s_pant_suma = 0;
    s_pant_pix  = 0;
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

/* ── 3 bis 2. La luz: alguien mueve el LEDC sin pedir brillo ──────────────
 *
 * Todo el firmware que cambia el brillo pasa por bsp_display_brightness_set(),
 * que deja en el BSP el duty que acaba de escribir. Aqui se compara el duty
 * REAL del LEDC con ese valor cada 2 ms: si no coinciden durante 100 ms
 * seguidos, alguien ha tocado el periferico sin pasar por el BSP. (Se lee el
 * registro, no se escribe nada.)
 *
 * El margen de 100 ms NO es un capricho: la primera version comparaba el
 * contador de escrituras con el duty y saltaba en falso justo cuando el BSP
 * estaba escribiendo (se leia el duty viejo con el contador nuevo). Con el
 * margen, una escritura legitima siempre entra dentro de la ventana.
 */
static uint32_t s_luz_desacuerdo;    /* tics seguidos con el duty distinto al pedido */
static uint32_t s_luz_avisos;

static void perf_mira_luz(void)
{
    uint32_t duty = (uint32_t)ledc_get_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL_NUM);
    if (duty == bsp_display_brightness_duty()) {
        s_luz_desacuerdo = 0;
        return;
    }
    uint32_t n = ++s_luz_desacuerdo;
    /* 50 tics de 2 ms = 100 ms; luego uno cada 5 s mientras dure. */
    if ((n == 50 || (n % 2500 == 0)) && s_luz_avisos < 50) {
        s_luz_avisos++;
        ESP_LOGW(TAG, "LUZ: el LEDC da duty %u pero el BSP dejo %u (brillo %d%%, pad GPIO%d=%d); "
                 "ese cambio no lo ha pedido el firmware",
                 (unsigned)duty, (unsigned)bsp_display_brightness_duty(),
                 bsp_display_brightness_get(), (int)BSP_LCD_BACKLIGHT,
                 gpio_get_level(BSP_LCD_BACKLIGHT));
    }
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
 * RESULTADO (10-nov-2026): descartado en el banco -- cero underruns con la
 * pantalla dando flashes. Vuelve a medirse el 11-nov porque el destello sale
 * en DOS unidades identicas: lo comun es el software, y esto es la rama del
 * driver que quedo sin cubrir.
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

/* ── 3 ter bis. Contador de flancos del PWM de la luz (GPIO23) ────────────
 *
 * 12-nov-2026. El video del destello (10x ralentizado) dice tres cosas: dura
 * ~7 ms (menos de medio fotograma), la imagen sigue con el mismo contraste y el
 * brillo sube por igual en todas las filas. Eso descarta contenido de LVGL y
 * descarta el dato de reserva del DSI: es LUZ.
 *
 * La pregunta que queda es si el pulso nace DENTRO del chip (el pin 23 se queda
 * clavado un instante) o FUERA (driver del LED, panel o su alimentacion). El
 * registro del LEDC no lo cuenta si el fallo es del pad, asi que aqui se cuentan
 * los flancos del pin de verdad: el PWM va a 5 kHz, o sea ~20 flancos cada 2 ms.
 * Si en una ventana de 2 ms con brillo intermedio no hay NINGUN flanco, el pin
 * se ha quedado clavado y se apunta con fecha.
 *
 * (Antes se probo un canal RMT en modo escucha, que ademas mide el ancho de cada
 * nivel: creaba el canal y se habilitaba bien, pero no capturaba ni una vez -- el
 * RMT no arranca a capturar si no ve el primer flanco, y no se pudo confirmar
 * que el pin se viera como entrada. Con el contador de flancos no hay duda.)
 */
static volatile uint32_t s_pad_flancos;
static uint32_t s_pad_flancos_previo;
static uint32_t s_pad_avisos;
static int64_t  s_pad_ultimo_us;

static void IRAM_ATTR perf_pad_isr(void *arg)
{
    (void)arg;
    s_pad_flancos++;
}

static void perf_vigila_pad(void)
{
    /* OJO, ESTO SE HIZO MAL LA PRIMERA VEZ (12-nov): se llamo a gpio_config()
     * con GPIO_MODE_INPUT, y eso APAGA LA SALIDA del pad: el LEDC se queda sin
     * pin y la luz sin PWM. Ahora solo se habilita la ENTRADA (para ver los
     * flancos) y se deja la salida del LEDC como esta. */
    esp_err_t err = gpio_input_enable(BSP_LCD_BACKLIGHT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PAD: no pude habilitar la entrada de GPIO%d (%s)",
                 (int)BSP_LCD_BACKLIGHT, esp_err_to_name(err));
        return;
    }
    gpio_set_intr_type(BSP_LCD_BACKLIGHT, GPIO_INTR_ANYEDGE);
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "PAD: sin servicio de interrupciones de GPIO (%s)", esp_err_to_name(err));
        return;
    }
    if (gpio_isr_handler_add(BSP_LCD_BACKLIGHT, perf_pad_isr, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "PAD: no pude enganchar la interrupcion de GPIO%d", (int)BSP_LCD_BACKLIGHT);
        return;
    }
    ESP_LOGW(TAG, "PAD: contando flancos de GPIO%d (PWM de 5 kHz = ~20 cada 2 ms)",
             (int)BSP_LCD_BACKLIGHT);
}

/* Se llama desde el tic rapido (2 ms). OJO: el tic puede dispararse dos veces
 * seguidas (el temporizador de 2 ms no garantiza separacion si el callback se
 * retrasa), y entonces la ventana entre dos muestras puede ser de microsegundos
 * y no dar ni un flanco. Por eso solo se juzga cuando han pasado al menos
 * 1,5 ms desde la muestra anterior: a 5 kHz eso son ~15 flancos. */
static void perf_mira_pad(void)
{
    int64_t ahora_us = esp_timer_get_time();
    if (ahora_us - s_pad_ultimo_us < 1500) return;
    s_pad_ultimo_us = ahora_us;

    uint32_t ahora = s_pad_flancos;
    uint32_t duty  = bsp_display_brightness_duty();
    bool intermedio = (duty > 0 && duty < 1023);
    if (intermedio && ahora == s_pad_flancos_previo && s_pad_avisos < 50) {
        s_pad_avisos++;
        ESP_LOGW(TAG, "PAD: GPIO%d sin flancos en 1,5 ms con brillo %d%% (duty %u) -- pin clavado; van %u",
                 (int)BSP_LCD_BACKLIGHT, bsp_display_brightness_get(),
                 (unsigned)duty, (unsigned)s_pad_avisos);
    }
    s_pad_flancos_previo = ahora;
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
        "\"tareas\":[",
        esp_app_get_description()->version,
        (long long)(ahora / 1000000),
        motivo_reinicio(esp_reset_reason()),
        (unsigned)heap_libre, (unsigned)heap_mayor, (unsigned)heap_minimo,
        (unsigned)s_sd_n, (unsigned)s_sd_fallos,
        (long long)(s_sd_n ? s_sd_suma_us / s_sd_n : 0), (long long)s_sd_max_us,
        (unsigned)s_flush_n,
        (long long)(s_flush_n ? s_flush_suma_us / s_flush_n : 0), (long long)s_flush_max_us);

    for (uint32_t i = 0; i < n_orden && w + 64 < n; i++) {
        w += (size_t)snprintf(out + w, n - w,
            "%s{\"n\":\"%s\",\"cpu\":%u,\"pila\":%u,\"nucleo\":%d}",
            i ? "," : "", orden[i].nombre, (unsigned)orden[i].cpu_pct,
            (unsigned)orden[i].pila_libre, orden[i].nucleo);
    }
    w += (size_t)snprintf(out + w, (w < n) ? n - w : 0, "]}");
    return w;
}

/* ── 5 bis. Resumen periodico SIN tocar el planificador (12-nov-2026) ──────
 *
 * POR QUE ESTO CAMBIA. Hasta hoy, cada 10 s se llamaba a perf_muestra(), que
 * hace `uxTaskGetSystemState()`: esa funcion PARALIZA EL PLANIFICADOR y recorre
 * todas las tareas. En otro proyecto con ESTE MISMO chip y ESTE MISMO tipo de
 * pantalla (ESP32-P4 + MIPI-DSI, github.com/jantielens/esp32-macropad, issue 7)
 * esta exactamente esa llamada esta documentada como LA CAUSA de los destellos:
 * el puente DPI no consigue leer el framebuffer de PSRAM a tiempo (underrun) y
 * el panel muestra su color de reserva durante un instante. Su arreglo fue
 * quitar `uxTaskGetSystemState()` del camino periodico y medir la CPU con los
 * contadores de ralentí de cada nucleo.
 *
 * Aqui se hace lo mismo: el tic de 10 s ya NO muestrea tareas ni recorre el
 * heap. El resumen periodico solo imprime contadores que ya estan en RAM.
 * La foto completa (CPU por tarea, bloque mayor del heap) se sigue pudiendo
 * pedir a mano en /api/perf, que es una accion deliberada y no periodica.
 */
static void perf_log_ligero(void)
{
    ESP_LOGI(TAG, "resumen: heap libre=%u | sd n=%u media=%lldms max=%lldms | "
             "lvgl volcados=%u media=%lldus max=%lldus | pantallas=%u claras=%u "
             "regiones=%u | luz duty=%u escrituras=%u avisos=%u | pad flancos=%u "
             "avisos=%u | dsi underruns=%u | pwm=%d%%",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)s_sd_n,
             (long long)(s_sd_n ? s_sd_suma_us / s_sd_n / 1000 : 0),
             (long long)(s_sd_max_us / 1000),
             (unsigned)s_flush_n,
             (long long)(s_flush_n ? s_flush_suma_us / s_flush_n : 0),
             (long long)s_flush_max_us,
             (unsigned)s_pant_completas, (unsigned)s_bandas_claras,
             (unsigned)s_regiones,
             (unsigned)bsp_display_brightness_duty(),
             (unsigned)bsp_display_brightness_writes(),
             (unsigned)s_luz_avisos,
             (unsigned)s_pad_flancos, (unsigned)s_pad_avisos,
             (unsigned)s_dsi_underruns,
             bsp_display_brightness_get());
}

/* ── 6. Arranque ─────────────────────────────────────────────────────────── */
static void perf_tick(void *arg)
{
    (void)arg;
    static uint32_t tics;
    ++tics;
    /* NO se muestrea aqui (13-nov-2026): perf_muestra() llama a
     * uxTaskGetSystemState(), que PARALIZA EL PLANIFICADOR para copiar la lista
     * de tareas. En el proyecto jantielens/esp32-macropad, con este mismo chip
     * y este mismo tipo de panel MIPI-DSI, esa llamada esta documentada como la
     * causa de los destellos (el puente DPI no llega a leer el framebuffer a
     * tiempo). Aqui entro el 09-nov-2026 y los fogonazos aparecen por primera
     * vez en el historial ese mismo dia por la tarde. Se quita del camino
     * periodico y se deja SOLO para cuando alguien pide /api/perf, que es una
     * accion deliberada y no una vez cada 10 s.
     *
     * La foto de CPU por tarea sigue disponible en /api/perf (perf_json la
     * muestrea si esta vieja). El resumen de cada 10 min ya no la lleva. */
    /* Resumen al log cada 10 min (60 muestras de 10 s). */
    if (tics % 60 == 0) perf_log_ligero();
}

static void perf_tick_rapido(void *arg)
{
    (void)arg;
    perf_mira_dsi();
    perf_mira_luz();
    perf_mira_pad();
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

    /* Contador de underruns del DSI, cada 2 ms, con la interrupcion del driver
     * apagada (su ISR borra el bit y no se ve nada desde una tarea). */
    perf_dsi_quita_isr();
    static esp_timer_handle_t t_dsi;
    const esp_timer_create_args_t args_dsi = {
        .callback = perf_tick_rapido, .name = "perf_dsi",
    };
    if (esp_timer_create(&args_dsi, &t_dsi) != ESP_OK) {
        ESP_LOGW(TAG, "no pude crear el temporizador del DSI");
    } else {
        esp_timer_start_periodic(t_dsi, 2 * 1000);
    }
    perf_vigila_pad();
    perf_muestra();   /* primera referencia para el primer % */
    ESP_LOGI(TAG, "medidas activas (CPU por tarea, heap, SD, LVGL); /api/perf");
}
