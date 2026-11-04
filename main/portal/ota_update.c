/* Actualizacion del firmware por Wi-Fi.
 *
 * Para que sirve: hasta ahora, actualizar exigia cable USB y el portatil al lado
 * de la pantalla. Con esto se sube el fichero desde el navegador del movil,
 * estando en la autocaravana.
 *
 * Como funciona: la flash tiene DOS huecos de firmware (ver partitions.csv). Lo
 * que se sube se escribe en el que NO se esta usando, y solo se marca para
 * arrancar si ha llegado entero y valido. Si se corta la subida, se queda todo
 * como estaba: no se puede quedar a medias.
 *
 * Va en su propio fichero y no dentro de config_server.c, que ya tiene ~1.000
 * lineas (1.009 con wc -l el 2-oct-2026).
 */
#include "ota_update.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "watchdog.h"
#include "camera.h"
#include "ui.h"
#include "esp_lvgl_port.h"
#include "log_capture/log_capture.h"
#include "display.h"   /* bsp_display_brightness_set/get */
#include "config_server_internal.h"
#include "config_server_viaje.h"   /* viaje_flush_antes_de_reiniciar() */

static const char *TAG = "ota";

/* Trozo de lectura. 4 KB va sobrado y no come RAM: el cuello de botella es el
 * Wi-Fi, no la flash. */
#define OTA_CHUNK 4096

/* EL TASK WATCHDOG DE ESP-IDF DURANTE LA OTA: SE LE DA MAS MARGEN, NO SE
 * BORRA NADIE (29-oct-2026; medido en la placa).
 *
 * Antes se DESUSCRIBIAN del TWDT las dos tareas IDLE y la de la camara, y se
 * volvian a suscribir al acabar. El borrado de la flash y su escritura son
 * operaciones largas -- y la tarea IDLE, al no estar suscrita, hace que su
 * propio esp_task_wdt_reset() interno devuelva ESP_ERR_NOT_FOUND, que el IDF
 * imprime como ERROR EN CADA PASADA. Resultado medido en una OTA real de
 * 3,3 MB: 56 segundos de "task_wdt: esp_task_wdt_reset(707): task not found" a
 * ~150 por segundo, o sea el puerto de serie saturado mientras se graba (y el
 * mismo ruido por parte de la camara, que tambien late a mano).
 *
 * Ahora no se borra a nadie: se ESTIRA el plazo del TWDT mientras dura la OTA
 * (de 10 s a 2 min) y se devuelve al acabar, tanto en el exito como en todos
 * los fallos. Se mantiene entero el watchdog de verdad -- si la placa se cuelga
 * durante la OTA, sigue reiniciando, solo que con mas margen -- y desaparece el
 * torrente de errores que ademas ralentizaba la grabacion.
 *
 * watchdog_suspend_ex() se queda: es el watchdog DEL PROYECTO (el de la UI
 * congelada), que es otro mecanismo. */
#define OTA_TWDT_TIMEOUT_MS   (120 * 1000)
#define OTA_TWDT_NORMAL_MS    (10 * 1000)

static void ota_twdt_plazo(uint32_t ms)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = ms,
        /* LAS DOS IDLE SIGUEN SUSCRITAS, solo se les da mas plazo. Si se
         * desuscriben, su propio reset interno falla y el IDF lo grita en cada
         * pasada de la IDLE -- que es justo el torrente que se quiere quitar. */
        .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
        .trigger_panic  = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "TWDT: no pude poner el plazo en %u ms (%s)", (unsigned)ms,
                 esp_err_to_name(err));
}

static void ota_wdt_suspend(bool suspend)
{
    /* dueno = true: la OTA es uno de los DOS usuarios de watchdog_suspend (el
     * otro es el carrusel de capturas). Hay que decir quien es: solo el mismo
     * dueno puede reanudar (asi el carrusel no reanuda por debajo de una OTA) y
     * una suspension colgada caduca sola a los 10 minutos. */
    watchdog_suspend_ex(suspend, true);
    ota_twdt_plazo(suspend ? OTA_TWDT_TIMEOUT_MS : OTA_TWDT_NORMAL_MS);
}

/* Reinicio diferido: hay que contestar al navegador ANTES de reiniciar, si no el
 * movil se queda esperando y parece que ha fallado. */
static void ota_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "actualizacion aplicada: reiniciando");
    /* Que el arranque siguiente sepa que este reinicio lo pedia una
     * actualizacion: no es una averia y no se apunta como ultimo reinicio. */
    watchdog_marca_reinicio_pedido();
    /* Con un viaje abierto, hasta 5 min de ruta vivian en RAM y este reinicio
     * se los llevaba (auditoria del 13-oct-2026). */
    viaje_flush_antes_de_reiniciar();
    esp_restart();
}

static void ota_reboot_en(uint32_t ms)
{
    const esp_timer_create_args_t args = {
        .callback = ota_reboot_cb,
        .name = "ota_reboot",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_err_t err = esp_timer_start_once(t, (uint64_t)ms * 1000);
        if (err != ESP_OK) ESP_LOGW(TAG, "timer de reinicio OTA no arranco: %s", esp_err_to_name(err));
    }
}

/* ── Congelar la pantalla mientras se graba la flash ─────────────────────────
 * El parpadeo negro/azul del aviso "Actualizando firmware" no se puede quitar
 * por configuracion en esta placa: el chip de flash es un BOYA y el driver de
 * IDF dice que no soporta flash-suspend
 * (components/spi_flash/spi_flash_chip_boya.c: "flash-suspend is not supported";
 * el chip es mfr 0x68 dev 0x4018, visto con esptool flash_id). Sin suspend, cada
 * borrado/programacion deja al panel sin poder leer su framebuffer.
 *
 * Lo que SI se puede hacer (idea del usuario, 6-oct-2026) es que, mientras se
 * graba, NADA escriba en pantalla: se coge el lock del port LVGL y no se suelta
 * hasta terminar. Asi la tarea de LVGL no corre, no hay redibujados ni cambios
 * de buffer, y el aviso se queda fijo tal cual estaba: lo que se ve es un
 * fotograma congelado, no uno que se reescribe a trozos. Ademas deja de haber
 * reloj/timers tocando el framebuffer justo cuando la flash tiene el bus
 * ocupado. */
static bool s_ui_congelada = false;

/* refrescar=true: pinta AHORA, con el lock cogido, lo que se acabe de crear en
 * el overlay. Sin esto se congelaba el dibujo ANTERIOR y el mensaje no se veia
 * nunca (lo vio el usuario el 6-oct-2026: "sin mensaje de actualizando
 * firmware"). En el exito se llama con false a proposito: el "Firmware
 * instalado" lo pinta la tarea de LVGL un instante despues, ya descongelada. */
static void ota_ui_congelar(bool refrescar)
{
    /* 1 s de margen: si la tarea de LVGL esta terminando un fotograma, que
     * acabe. Si no se consigue, se sigue igual (peor, pero no se aborta). */
    s_ui_congelada = lvgl_port_lock(1000);
    if (s_ui_congelada && refrescar) lv_refr_now(NULL);
    ESP_LOGI(TAG, "pantalla congelada durante la grabacion%s",
             s_ui_congelada ? "" : " (NO se pudo coger el lock de LVGL)");
}

static void ota_ui_descongelar(void)
{
    if (!s_ui_congelada) return;
    lvgl_port_unlock();
    s_ui_congelada = false;
    /* La OTA dura ~40 s con la pantalla apagada, asi que el salvapantallas ha
     * podido entrar (en el log se veia "Backlight 25%" justo despues de
     * devolver el brillo, y el "Firmware instalado" salia atenuado). Esto lo
     * despierta y devuelve el brillo del usuario. */
    ui_notify_user_activity();
    ESP_LOGI(TAG, "pantalla descongelada y despertada");
}


/* ── La pantalla, a oscuras mientras se graba la flash ───────────────────────
 * Con la UI congelada el parpadeo no viene de la UI: el panel se queda sin poder
 * leer su framebuffer mientras la flash se borra/escribe (underrun del DSI). En
 * esta placa no hay ajuste que lo quite: el chip de flash es un BOYA y el driver
 * de IDF dice que no soporta flash-suspend, el DPI del P4 no tiene bounce buffer
 * y el framebuffer (1,2 MB) no cabe en RAM interna. Lo unico que queda es que no
 * se vea: se enseña el aviso 3 s con la pantalla encendida y SIN tocar la flash
 * (limpio, sin parpadeo) y despues se apaga el brillo durante la grabacion. El
 * brillo se devuelve en todas las salidas. */
static int  s_brillo_previo = -1;
static bool s_pantalla_apagada = false;

/* Apaga SIEMPRE (no se salta si ya hay brillo guardado: si no, el segundo
 * apagado -- el de la grabacion -- no hacia nada y la OTA se grababa con la
 * pantalla encendida. Visto en el log del 6-oct-2026: "Backlight 0%" y 250 ms
 * despues "Backlight 25%", y de ahi no volvia a apagarse). */
static void ota_pantalla_oscura(void)
{
    if (s_brillo_previo < 0) s_brillo_previo = bsp_display_brightness_get();
    bsp_display_brightness_set(0);
    s_pantalla_apagada = true;
    ESP_LOGW(TAG, "pantalla a oscuras (brillo guardado %d%%)", s_brillo_previo);
}

/* Vuelve a encender con el brillo que se guardo (sin olvidarlo: el apagado
 * definitivo y la restauracion final lo siguen necesitando). */
static void ota_pantalla_encender(void)
{
    /* Atenuado a proposito: con la pantalla a plena luz el parpadeo de la subida
     * (trafico de PSRAM, no se puede parar) se nota, y a ~20% apenas se ve
     * mientras el aviso se sigue leyendo. El brillo bueno se devuelve al final. */
    int b = s_brillo_previo >= 20 ? 20 : s_brillo_previo;
    if (b >= 0) bsp_display_brightness_set(b);
    s_pantalla_apagada = false;
}

static void ota_pantalla_restaurar(void)
{
    if (s_brillo_previo < 0) return;
    bsp_display_brightness_set(s_brillo_previo);
    ESP_LOGW(TAG, "brillo restaurado al %d%% (apagada=%d)", s_brillo_previo,
             (int)s_pantalla_apagada);
    s_brillo_previo = -1;
    s_pantalla_apagada = false;
}

/* Enciende la pantalla para que el aviso se LEA, y deja guardado el brillo que
 * tenia el usuario. Es el paso que faltaba: el aviso se pintaba con el brillo
 * que hubiera en ese momento (25% del salvapantallas, o 0% si el modo noche lo
 * habia apagado) y el usuario no veia ningun mensaje. Se pone el brillo alto
 * solo mientras se lee; en cuanto empieza la grabacion se apaga otra vez. */
static void ota_pantalla_aviso(void)
{
    if (s_brillo_previo < 0) s_brillo_previo = bsp_display_brightness_get();
    int b = s_brillo_previo > 70 ? s_brillo_previo : 70;
    bsp_display_brightness_set(b);
    s_pantalla_apagada = false;
    ESP_LOGW(TAG, "aviso en pantalla: brillo al %d%% (el del usuario era %d%%)",
             b, s_brillo_previo);
}

/* Aviso previo: la pagina web lo pide ANTES de subir nada. Asi el mensaje se
 * pinta con la placa tranquila (sin subida por Wi-Fi, sin tocar flash) y se lee
 * LIMPIO, sin el parpadeo que salia cuando se encendia ya con la subida en
 * marcha. La pagina espera 5 s antes de subir; despues la pantalla se apaga y no
 * se vuelve a encender hasta el "Firmware instalado" del final. */
esp_err_t ota_aviso_post(httpd_req_t *req)
{
    /* CON CONTRASENA, y antes NO la tenia (auditoria de seguridad del
     * 13-oct-2026): este endpoint no escribe nada, pero pinta en la pantalla un
     * "Actualizando firmware, no apagues" y enciende el aviso de OTA. Sin auth,
     * cualquiera dentro del Wi-Fi podia dejar la pantalla del salón enseñando
     * una actualizacion que no existe, todas las veces que quisiera. El /ota de
     * verdad ya la pedia; esto es la misma puerta. La pagina del portal la
     * manda despues de autenticarse, asi que el navegador trae la cabecera
     * igual que para el resto de la pagina.
     *
     * Y CON LA CLAVE DE ADMINISTRACION, NO LA DEL PORTAL (auditoria del
     * 27-oct-2026, M6): /ota es estricto, pero este aviso se quedo en el nivel
     * abierto -- el que llevan la app y las cabinas --. Pintar la pantalla de
     * "actualizando firmware" no es tan grave como escribir el firmware, pero es
     * el mismo camino y la misma puerta. */
    REQUIRE_AUTH_STRICT(req);
    ota_pantalla_aviso();
    ui_notify_user_activity();   /* por si el salvapantallas lo tenia atenuado */
    /* Si hay un viaje abierto se dice EN LA PANTALLA, no solo en el log
     * (auditoria del 13-oct-2026: una OTA a mitad de viaje reiniciaba la placa
     * sin avisar de nada). La ruta en RAM se vuelca antes de reiniciar (ver
     * viaje_flush_antes_de_reiniciar), pero el viaje se queda abierto y hay que
     * saberlo: quien tiene que cerrarlo es el cuaderno de la cabina. */
    char vj[64];   /* CARPETA_MAX de config_server_viaje.c es privado */
    if (viaje_abierto_nombre(vj, sizeof(vj))) {
        ESP_LOGW(TAG, "OTA con el viaje %s ABIERTO: se volcara la ruta antes de "
                      "reiniciar, pero el viaje sigue sin cerrar", vj);
        ui_ota_overlay_show("Actualizando firmware\n\n"
                            "OJO: hay un viaje abierto.\n"
                            "Se guarda la ruta, pero el viaje\n"
                            "seguira sin cerrar al reiniciar.\n\n"
                            "No apagues la pantalla");
    } else {
        ui_ota_overlay_show("Actualizando firmware\n\nNo apagues la pantalla");
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t ota_update_receive(httpd_req_t *req)
{
    /* EL AVISO PUESTO POR /ota_aviso SE QUITA EN TODAS LAS SALIDAS (auditoria del
     * 27-oct-2026, M6). La pagina pide el aviso, espera 5 s y sube; si la subida
     * no llega o no empieza (sin particion, sin adjunto, no cabe), el overlay a
     * pantalla completa se quedaba TAPANDO el tactil hasta cortar la corriente:
     * solo lo quitaba el camino bueno. Aqui se quita en las tres salidas de
     * arriba; y el que confirma que empieza de verdad (mas abajo) cancela el
     * temporizador de seguridad de ui.c. */
    const esp_partition_t *destino = esp_ota_get_next_update_partition(NULL);
    if (!destino) {
        ESP_LOGE(TAG, "no hay hueco de actualizacion (reparto de flash antiguo?)");
        ui_ota_overlay_hide();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "Esta pantalla no admite actualizacion por Wi-Fi: hay que grabarla "
            "una vez por cable con el reparto de memoria nuevo.");
        return ESP_OK;
    }
    if (req->content_len <= 0) {
        ui_ota_overlay_hide();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "No has adjuntado ningun fichero.");
        return ESP_OK;
    }
    if ((size_t)req->content_len > destino->size) {
        ui_ota_overlay_hide();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "El fichero no cabe: no parece un firmware valido.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "recibiendo %d bytes -> particion '%s'",
             req->content_len, destino->label);

    /* esp_ota_begin borra de golpe el hueco de destino (varios segundos): sin
     * esto el watchdog de UI congelada (watchdog.c) puede saltar y reiniciar
     * la placa a medias, antes de recibir un solo byte. Se reanuda en TODAS
     * las salidas de aqui en adelante, exito o fallo. */
    ota_wdt_suspend(true);
    /* Tapa el parpadeo de pantalla durante la OTA (no lo arregla, ver ui.c).
     * Se oculta en TODAS las salidas de fallo de aqui en adelante; en exito
     * se deja puesto (con el texto cambiado) porque la placa reinicia sola. */
    /* 1) El aviso se pinta CON LA PANTALLA ENCENDIDA y se congela: asi queda
     *    completo en el framebuffer antes de tocar la flash. La pagina ya lo
     *    pidio antes (ota_aviso_post) y ha dejado 5 s para leerlo; se repite
     *    aqui por si la subida llega sin aviso (curl, o un aviso que se perdio).
     * 2) Y solo entonces se apaga: toda la grabacion (borrado + escritura) va
     *    con la pantalla a oscuras, que es donde salia el parpadeo. */
    ota_pantalla_aviso();
    ui_ota_overlay_show("Actualizando firmware\n\nNo apagues la pantalla");
    /* Y AHORA SE CANCELA EL TEMPORIZADOR DE SEGURIDAD (29-oct-2026, medido en la
     * placa): ui_ota_overlay_show() lo REARMA cada vez que se llama, asi que
     * cancelarlo antes de volver a pintar el aviso no servia de nada -- a los
     * 30 s de subida el mensaje "Actualizando firmware" DESAPARECIA de la
     * pantalla con la grabacion en marcha (visto en una OTA de 56 s). Tiene que
     * ir despues del show. */
    ui_ota_overlay_confirm();
    ota_ui_congelar(true);      /* pinta el aviso y congela */
    ota_pantalla_oscura();

    /* Deja rastro en la SD de que esta OTA paso por aqui (el log de cada
     * arranque se guarda en el boot; el de esta sesion, si no, se pierde). */
    esp_err_t err_log = log_capture_autosave_now(20);
    ESP_LOGW(TAG, "OTA: aviso congelado y pantalla a oscuras, %d bytes (log a SD: %s)",
             req->content_len, esp_err_to_name(err_log));

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(destino, req->content_len, &ota);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo preparar la actualizacion.");
        return ESP_OK;
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Sin memoria para la actualizacion.");
        return ESP_OK;
    }

    int restante = req->content_len;
    bool fallo = false;
    /* Tope de esperas seguidas. Antes se reintentaba SIN limite: si el movil se
     * iba del Wi-Fi a mitad y el socket se quedaba a medias, esto no salia nunca
     * del bucle y dejaba ocupada la unica tarea del servidor web (con la OTA
     * abierta). Con recv_wait_timeout=30 s, 4 esperas son ~2 min de silencio
     * antes de rendirse. 2026-07-26. */
    const int MAX_ESPERAS = 4;
    /* Ademas del tope de esperas SEGUIDAS, un plazo total absoluto: esperas
     * solo cuenta silencios consecutivos y se reinicia con CUALQUIER byte
     * que llegue, asi que un cliente goteando muy despacio a proposito (1
     * byte cada 25s) nunca disparaba el limite y podia tener esto ocupado
     * sin fin -- la unica tarea del httpd. Firmware entero por Wi-Fi puede
     * tardar de verdad, asi que el plazo es mas generoso que en /save: 10
     * min cubren una subida lenta legitima con margen. Detectado auditando
     * el 08-sep-2026 (mismo motivo que el plazo anadido a /save). */
    const int64_t plazo_us = 10LL * 60 * 1000000LL;
    const int64_t t0_us = esp_timer_get_time();
    int esperas = 0;
    while (restante > 0) {
        if (esp_timer_get_time() - t0_us > plazo_us) {
            ESP_LOGE(TAG, "OTA: plazo total agotado con %d bytes por recibir", restante);
            fallo = true;
            break;
        }
        const int pedir = (restante < OTA_CHUNK) ? restante : OTA_CHUNK;
        const int leido = httpd_req_recv(req, buf, pedir);
        if (leido <= 0) {
            /* HTTPD_SOCK_ERR_TIMEOUT: el movil va lento, se reintenta. */
            if (leido == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= MAX_ESPERAS) continue;
            if (leido == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "la subida lleva %d esperas seguidas sin datos: se corta",
                         esperas);
            }
            ESP_LOGE(TAG, "se corto la subida con %d bytes por recibir", restante);
            fallo = true;
            break;
        }
        esperas = 0;   /* han llegado datos: la cuenta de esperas SEGUIDAS se reinicia */
        if (esp_ota_write(ota, buf, leido) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fallo");
            fallo = true;
            break;
        }
        restante -= leido;
        /* Ceder la CPU tras cada chunk (~10 ms/700 chunks ~= 7 s extra en total).
         * Sin esto, con el movil en la misma LAN los datos llegan tan seguidos
         * que httpd_req_recv casi nunca bloquea y el nucleo que atiende esta
         * tarea queda monopolizado por la escritura/borrado de flash: el Task
         * Watchdog de ESP-IDF (distinto del watchdog.c del proyecto, y NO
         * cubierto por watchdog_suspend) puede pillar sin latido incluso a la
         * tarea IDLE de ese nucleo y reiniciar la placa a medias. Reproducido
         * 2026-08-13: "task_wdt ... cam_stream / IDLE0" con Reset reason
         * "Watchdog (TASK)". */
        vTaskDelay(1);
    }
    free(buf);

    if (fallo) {
        esp_ota_abort(ota);
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
            "La subida se ha cortado. NO se ha tocado el firmware actual: "
            "la pantalla sigue funcionando igual. Vuelve a intentarlo.");
        return ESP_OK;
    }

    /* esp_ota_end valida la imagen (cabecera y firma del binario). */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ota_wdt_suspend(false);
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
            "El fichero no es un firmware valido para esta pantalla. "
            "No se ha cambiado nada.");
        return ESP_OK;
    }

    /* ── ¿Y ESTE FIRMWARE ES DE ESTA PLACA? (27-oct-2026) ──────────────────
     *
     * esp_ota_end() solo mira que la imagen este BIEN FORMADA: un .bin de la
     * pantalla de 5" (ESP32-S3) tambien lo esta. Sin esta comprobacion se
     * marcaba para arrancar, la placa reiniciaba, el bootloader lo rechazaba
     * (no es una imagen de ESP32-P4) y volvia al firmware anterior.
     *
     * Paso de verdad el 27-oct: CUATRO subidas del cabina5-v1.44 al /ota de la
     * P4, con sus cuatro reinicios y un "connection abort" en la app que
     * parecia un fallo de red. Aqui se compara el proyecto que va DENTRO del
     * .bin con el que corre; si no es el mismo se dice, y no se toca ni la
     * particion de arranque ni el reinicio. */
    esp_app_desc_t nueva;
    if (esp_ota_get_partition_description(destino, &nueva) == ESP_OK) {
        const esp_app_desc_t *actual = esp_app_get_description();
        const char *proyecto_actual = actual ? actual->project_name : "?";
        if (strcmp(nueva.project_name, proyecto_actual) != 0) {
            const char *cual = NULL;
            if (strcmp(nueva.project_name, "cabina5") == 0)        cual = "la pantalla de 5\"";
            char msg[192];
            if (cual)
                snprintf(msg, sizeof(msg),
                         "Ese .bin es de %s, no de este aparato: subelo en la "
                         "pagina Pantallas (http://192.168.4.1:8081/pantallas). "
                         "No se ha cambiado nada.", cual);
            else
                snprintf(msg, sizeof(msg),
                         "Ese .bin es del proyecto '%s' y este aparato es '%s': "
                         "no es su firmware. No se ha cambiado nada.",
                         nueva.project_name, proyecto_actual);
            ota_pantalla_restaurar();
            ota_ui_descongelar();
            ui_ota_overlay_hide();
            ota_wdt_suspend(false);
            ESP_LOGW(TAG, "OTA RECHAZADA: el .bin es '%s' y esta placa es '%s'. "
                          "No se toca la particion de arranque.",
                     nueva.project_name, proyecto_actual);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, msg);
            return ESP_OK;
        }
    }

    err = esp_ota_set_boot_partition(destino);
    ota_wdt_suspend(false);
    if (err != ESP_OK) {
        ota_pantalla_restaurar();
        ota_ui_descongelar();
        ui_ota_overlay_hide();
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No se pudo activar la version nueva.");
        return ESP_OK;
    }

    /* Exito: se deja el overlay puesto (con el texto cambiado) hasta que
     * reinicie sola en 1,5 s — no hace falta ocultarlo. El orden importa: el
     * brillo se devuelve ANTES de descongelar, y el texto se cambia ANTES de
     * descongelar, para que la tarea de LVGL pinte el "Firmware instalado" ya
     * con la pantalla encendida y de una sola vez (antes se descongelaba con el
     * texto viejo y el aviso bueno salia tarde o no salia). */
    ota_pantalla_restaurar();
    ui_ota_overlay_show("Firmware instalado\n\nReiniciando...");
    ota_ui_descongelar();
    /* Segundo volcado del log, ahora que la grabacion ha terminado: deja en la SD
     * la sesion completa de la OTA (el primero se hace al empezar, para no
     * perderla si algo sale mal). */
    log_capture_autosave_now(20);
    ESP_LOGI(TAG, "actualizacion grabada en '%s'", destino->label);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset='utf-8'>"
        "<body style='font-family:sans-serif;padding:24px'>"
        "<h2>Actualizacion instalada</h2>"
        "<p>La pantalla se esta reiniciando con la version nueva. "
        "Tardara unos 20 segundos.</p>"
        "<p>Si algo hubiera ido mal, arrancaria sola con la version anterior.</p>"
        "</body>");

    /* 1,5 s para que al movil le de tiempo a recibir la respuesta. */
    ota_reboot_en(1500);
    return ESP_OK;
}

esp_err_t ota_update_page(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *destino = esp_ota_get_next_update_partition(NULL);

    /* El fichero se sube EN CRUDO con fetch(), no con un formulario normal: un
     * formulario lo envuelve con separadores MIME y habria que desenvolverlo en
     * la pantalla (mas codigo y mas cosas que fallen). Asi el servidor solo
     * tiene que escribir lo que recibe. */
    char html[3200];
    snprintf(html, sizeof(html),
        "<!doctype html><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;padding:24px;max-width:560px'>"
        "<h2>Actualizar la pantalla</h2>"
        "<p>Version instalada: <b>%s</b><br>Compilada el %s</p>"
        "%s"
        "<p><a href='/pantallas'>Actualizar la pantalla de cabina (5\")</a></p>"
        "<p><input type='file' id='f' accept='.bin'></p>"
        "<p><button id='b' style='padding:12px 20px;font-size:16px'>Instalar</button></p>"
        "<p id='e'></p>"
        "<p style='background:#fff3cd;border:2px solid #ffb300;padding:12px;"
        "border-radius:8px'><b>No apagues la pantalla durante la actualizacion.</b> "
        "Primero saldra el aviso <b>en la pantalla</b> (5 segundos, para que lo "
        "leas); despues se pondra <b>negra</b> mientras se graba (es normal: asi "
        "no parpadea) y al final volvera sola con el mensaje de que ha "
        "terminado. Tarda menos de un minuto.</p>"
        "<p style='color:#666;font-size:14px'>Sube el fichero terminado en "
        "<code>-app.bin</code>. Si se corta, no pasa nada: sigue arrancando la "
        "version actual.</p>"
        "<script>"
        "var b=document.getElementById('b'),f=document.getElementById('f'),e=document.getElementById('e');"
        "b.onclick=function(){"
        " if(!f.files.length){e.textContent='Elige primero el fichero.';return;}"
        " b.disabled=true;e.textContent='Avisando a la pantalla...';"
        " var a=new XMLHttpRequest();"
        " a.onload=function(){setTimeout(subir,5000);};"
        " a.onerror=function(){setTimeout(subir,500);};"
        " a.open('POST','/ota_aviso');a.send();"
        "};"
        "function subir(){"
        " var x=new XMLHttpRequest();"
        " x.upload.onprogress=function(p){"
        "  if(p.lengthComputable){e.textContent='Subiendo... '+Math.round(p.loaded*100/p.total)+'%%';}};"
        " x.onload=function(){e.innerHTML=x.responseText;};"
        " x.onerror=function(){e.textContent='Se corto la conexion. La pantalla sigue igual.';b.disabled=false;};"
        " x.open('POST','/ota');"
        " x.setRequestHeader('Content-Type','application/octet-stream');"
        " x.send(f.files[0]);}"
        "</script>"
        "</body>",
        app ? app->version : "?",
        app ? app->date : "?",
        destino ? "" :
            "<p style='color:#b00'><b>Esta pantalla todavia no admite "
            "actualizacion por Wi-Fi.</b> Hay que grabarla una vez por cable con "
            "el reparto de memoria nuevo.</p>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}
