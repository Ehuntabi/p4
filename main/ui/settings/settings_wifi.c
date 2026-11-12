/* Pagina "Wi-Fi" de Ajustes: modo punto de acceso, SSID, contrasena y el aviso
 * de reinicio para aplicar cambios.
 *
 * Sale de settings_panel.c dentro del troceo por paginas (ver settings_common.h).
 * Es el mismo codigo movido de sitio, sin cambios de comportamiento.
 */
#include "settings_panel.h"
#include "settings_common.h"
#include "ui.h"
#include "ui/widgets/ui_card.h"
#include "ui/vigilancia/ausente_mode.h"
#include "fonts/fonts_es.h"
#include "audio_es8311.h"
#include "alerts.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "config_storage.h"
#include "portal/config_server.h"
#include "portal/config_server_auth.h"   /* cambiar la clave del portal */
#include "esp_random.h"
#include "victron_ble.h"
#include "display.h"
#include "esp_log.h"
#include "datalogger.h"
#include "battery_history.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ui/views/frigo_panel.h"
#include "ne185/ne185.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "camera.h"   /* camera_sd_bus_lock: coordinar SD con el GDMA de la camara */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "watchdog.h"
#include "config_backup.h"
#include "data/trip_computer.h"
#include <time.h>
#include "settings_common.h"

void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled);
/* Callbacks propios de esta pagina, definidos mas abajo. */
static void portal_page_cb(lv_event_t *e);
static void generar_clave_admin_cb(lv_event_t *e);

/* La etiqueta de la clave de ADMINISTRACION, para refrescarla al generarla. */
static lv_obj_t *s_c4_adm_lbl;

static void reactivate_portal_cb(lv_event_t *e);
static void ap_switch_cb(lv_event_t *e);
static void wifi_pintar_ip_ap(ui_state_t *ui);

/* Namespace NVS donde vive la configuracion Wi-Fi. */
#define WIFI_NAMESPACE "wifi"
static void wifi_save_cb(lv_event_t *e);
static void wifi_cancel_cb(lv_event_t *e);
static void campo_cambiado_cb(lv_event_t *e);
static void adm_ojito_cb(lv_event_t *e);
static void refresca_botones(void);
/* Lo que hay GUARDADO, para saber si los campos han cambiado de verdad. */
static char s_ssid_guardado[33];
static char s_pass_guardada[65];
static char s_adm_guardada[33];
static lv_obj_t *s_fila_botones;     /* fila Guardar/Cancelar (oculta si no hay cambios) */
/* Las dos tarjetas de arriba (punto de acceso y clave de administracion). Se
 * guardan para igualar su alto: al salir la fila, la primera crece y, si no se
 * toca la otra, quedan desalineadas. */
static lv_obj_t *s_card1;
static lv_obj_t *s_card4;
static lv_obj_t *s_adm_ta;           /* campo de la clave de administracion */
static lv_obj_t *s_adm_ojito;        /* boton para leerla escrita */
static lv_obj_t *s_ssid_ta;          /* campo del nombre de la red */
static lv_obj_t *s_pass_ta;          /* campo de la clave del punto de acceso */
static lv_obj_t *s_wifi_estado = NULL;   /* respuesta del boton Guardar */
void password_toggle_btn_event_cb(lv_event_t *e);

/* Pinta la IP del AP donde toca, o "--" si el AP esta apagado. */
static void wifi_pintar_ip_ap(ui_state_t *ui)
{
    if (!ui || !ui->wifi.ap_ip) return;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        lv_label_set_text_fmt(ui->wifi.ap_ip, "IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        lv_label_set_text(ui->wifi.ap_ip, "IP: -- (AP apagado)");
    }
}

static void ap_switch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = lv_event_get_target(e);
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    /* Guardar el nuevo estado en NVS y aplicarlo EN CALIENTE: antes esto solo
     * escribia NVS y sacaba un modal "hay que reiniciar". wifi_ap_init() ya
     * estaba escrita para re-invocarse (flags de init separados, netif creado
     * una sola vez, handlers idempotentes), asi que lo unico que faltaba era
     * llamarla — y hacerlo FUERA del hilo de LVGL, que es lo que hace
     * config_server_request_wifi_apply(). */
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", checked ? 1 : 0);
        esp_err_t err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "wifi enabled (settings) no persistio: %s", esp_err_to_name(err));
        nvs_close(h);
    }

    ui_wifi_set_enabled_cache(checked);   /* el icono de la barra cachea el flag */
    config_server_request_wifi_apply();
    wifi_pintar_ip_ap(ui);   /* encender/apagar cambia la IP del AP (o la deja en --) */
}

void create_wifi_settings_page(ui_state_t *ui, lv_obj_t *page_wifi,
                               const char *default_ssid,
                               const char *default_pass,
                               uint8_t ap_enabled)
{
    (void)ap_enabled;
    style_settings_scrollbar(page_wifi);
    /* Root container: ROW_WRAP + SPACE_BETWEEN para que las 2 primeras
     * cards (Punto de acceso + Pagina inicial portal) queden lado a lado
     * a pct(49), y la tercera (Reactivar portal web) ocupe linea entera
     * a pct(100) -- mismo patron que frigo_panel. */
    lv_obj_t *cont = lv_obj_create(page_wifi);
    lv_obj_set_width(cont, lv_pct(100));
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(cont, UI_PAD_4, 0);    /* 4-oct-2026: el relleno de pagina es UI_PAD_PAGE en TODAS las paginas de Ajustes */
    lv_obj_set_style_pad_gap(cont, 8, 0);   /* la pagina tiene que caber */

    /* === Card 1: Punto de acceso (mitad ancho, lado izdo) === */
    lv_obj_t *card1 = lv_obj_create(cont);
    lv_obj_set_width(card1, lv_pct(49));
    lv_obj_set_height(card1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card1, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, UI_COLOR_CYAN, 0);
    lv_obj_set_style_border_width(card1, 2, 0);
    lv_obj_set_style_radius(card1, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card1, 8, 0);  /* en 600 px sin deslizar  */
    lv_obj_set_style_pad_gap(card1, 8, 0);
    lv_obj_set_layout(card1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card1, LV_FLEX_FLOW_COLUMN);

    /* Interruptor + cabecera centrada: el switch va en la cabecera, equilibrado
     * por un espaciador (la tarjeta no crece). */
    lv_obj_t *sw_ap = lv_switch_create(card1);
    lv_obj_set_style_bg_color(sw_ap, UI_COLOR_CYAN, LV_STATE_CHECKED | LV_PART_INDICATOR);
    if (ap_enabled) lv_obj_add_state(sw_ap, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_ap, ap_switch_cb, LV_EVENT_VALUE_CHANGED, ui);

    lv_obj_t *card1_title = lv_label_create(card1);
    lv_obj_set_style_text_font(card1_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card1_title, UI_COLOR_CYAN, 0);
    lv_label_set_text(card1_title, LV_SYMBOL_WIFI "  Punto de acceso");
    ui_card_wrap_title_with(card1, card1_title, UI_COLOR_CYAN, sw_ap);

    /* Switch ON/OFF */
    ui->wifi.ap_enable = sw_ap;

    /* IP del punto de acceso: es la direccion a la que hay que entrar desde el
     * movil para el portal, asi que va AQUI, con el AP, y no en Acerca de
     * (sugerencia del usuario, 22-sep-2026). Se actualiza al crear la tarjeta y
     * cada vez que se enciende o apaga el AP. */
    ui->wifi.ap_ip = lv_label_create(card1);
    lv_obj_set_style_text_font(ui->wifi.ap_ip, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(ui->wifi.ap_ip, UI_COLOR_TEXT_DIM, 0);
    wifi_pintar_ip_ap(ui);   /* que se vea ya al entrar, no solo al tocar el interruptor */

    /* SSID row: label + input */
    lv_obj_t *ssid_row = lv_obj_create(card1);
    lv_obj_remove_style_all(ssid_row);
    lv_obj_set_width(ssid_row, lv_pct(100));
    lv_obj_set_height(ssid_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(ssid_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_ssid = lv_label_create(ssid_row);
    lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_ssid, "SSID:");

    ui->wifi.ssid = lv_textarea_create(ssid_row);
    lv_obj_set_style_text_font(ui->wifi.ssid, &lv_font_montserrat_24_es, 0);
    /* Altura fija y contenida: por defecto el campo venia alto y era lo que
     * estiraba la fila de arriba (idea del usuario, 22-sep-2026). */
    lv_obj_set_height(ui->wifi.ssid, UI_ROW_H);
    lv_textarea_set_one_line(ui->wifi.ssid, true);
    lv_obj_set_width(ui->wifi.ssid, 350);
    /* Tope 802.11: SSID max 32 caracteres. Sin esto se podia teclear un SSID
     * mas largo, que guardar_wifi_cb() (mas abajo) no rechazaba -- solo
     * validaba el minimo de la clave -- y wifi_ap_init() lo lee de NVS sin
     * revalidar. Detectado por el usuario el 09-sep-2026. */
    lv_textarea_set_max_length(ui->wifi.ssid, 32);
    lv_textarea_set_text(ui->wifi.ssid, default_ssid);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.ssid, ta_event_cb, LV_EVENT_READY, ui);
    s_ssid_ta = ui->wifi.ssid;
    /* SIN guardado automatico: lo escribe el boton "Guardar red" de abajo.
     * Guardar al perder el foco fue la mitad de un accidente serio (21-ago-2026):
     * la otra mitad rellenaba esta casilla con un nombre inventado a partir de
     * la MAC, y bastaba tocarla y salir para RENOMBRAR EL AP sin querer,
     * dejando al satelite sin red que buscar. Con un boton, no se guarda nada
     * hasta que se pide. */

    /* Password row */
    lv_obj_t *pass_row = lv_obj_create(card1);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_width(pass_row, lv_pct(100));
    lv_obj_set_height(pass_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(pass_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pass_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(pass_row, 8, 0);
    lv_obj_set_flex_align(pass_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_pass = lv_label_create(pass_row);
    lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_pass, "Password:");

    /* Lo que haya guardado; en blanco si aun no se ha generado (wifi_ap_init la
     * crea aleatoria al arrancar). Antes caia a DEFAULT_AP_PASSWORD y ensenaba
     * "12345678" aunque el AP usara otra cosa. 2026-07-26. */
    const char *ap_password = (default_pass && default_pass[0] != '\0') ? default_pass : "";

    /* Boton ojito a la IZQUIERDA del textarea */
    ui->wifi.password_toggle = lv_btn_create(pass_row);
    lv_obj_set_size(ui->wifi.password_toggle, 50, 40);
    lv_obj_add_event_cb(ui->wifi.password_toggle, password_toggle_btn_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_toggle = lv_label_create(ui->wifi.password_toggle);
    lv_label_set_text(lbl_toggle, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(lbl_toggle);

    ui->wifi.password = lv_textarea_create(pass_row);
    lv_obj_set_height(ui->wifi.password, UI_ROW_H);
    lv_obj_set_style_text_font(ui->wifi.password, &lv_font_montserrat_24_es, 0);
    lv_textarea_set_password_mode(ui->wifi.password, true);
    /* El caracter que se acaba de teclear se queda visible 5 s (LVGL trae 1,5):
     * la clave del AP es larga y aleatoria y con el temporizador corto no da
     * tiempo a comprobar lo escrito. El boton del ojo sigue estando para verla
     * entera. Mismo criterio que en el editor de las cabinas (27-oct-2026). */
    lv_textarea_set_password_show_time(ui->wifi.password, 5000);
    lv_textarea_set_one_line(ui->wifi.password, true);
    lv_obj_set_width(ui->wifi.password, 280);
    /* Tope WPA2-PSK: clave max 63 caracteres. Mismo motivo que el del SSID
     * de arriba. */
    /* 32, no 63 (05-nov-2026): esta clave es tambien la del portal abierto y
     * ahi los buffers son de 33. Dejarla teclear y rechazarla al guardar es
     * pedirle al usuario que se equivoque. */
    lv_textarea_set_max_length(ui->wifi.password, 32);
    lv_textarea_set_text(ui->wifi.password, ap_password);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(ui->wifi.password, ta_event_cb, LV_EVENT_READY, ui);
    s_pass_ta = ui->wifi.password;
    /* SIN guardado automatico, igual que el SSID: manda el boton de abajo. */
    /* En cuanto se toca cualquiera de los dos campos, se mira si hay cambios y
     * se enseña (o se esconde) la fila de Guardar/Cancelar. */
    lv_obj_add_event_cb(ui->wifi.ssid, campo_cambiado_cb, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->wifi.password, campo_cambiado_cb, LV_EVENT_VALUE_CHANGED, ui);
    /* Lo guardado, para comparar: el SSID y la clave que han llegado aqui son
     * los de NVS (los pasa ui.c), asi que son la referencia. */
    snprintf(s_ssid_guardado, sizeof(s_ssid_guardado), "%s", default_ssid ? default_ssid : "");
    snprintf(s_pass_guardada, sizeof(s_pass_guardada), "%s", default_pass ? default_pass : "");
    {   /* y la de administracion, que se lee ahora mismo de NVS */
        char u[33] = {0}, a[33] = {0};
        config_server_get_admin_credentials(u, sizeof(u), a, sizeof(a));
        snprintf(s_adm_guardada, sizeof(s_adm_guardada), "%s", a);
    }

    /* Boton de guardar, debajo de los campos. Escribe las DOS claves en la NVS;
     * la de la red entra cuando se reinicie la radio (ver wifi_save_cb: aqui NO
     * se reaplica el AP en caliente, que dejaba la red muda hasta reiniciar). */
    /* FILA GUARDAR / CANCELAR, OCULTA HASTA QUE HAY UN CAMBIO (5-nov-2026,
     * peticion del dueño): antes el boton estaba siempre ahi, se pulsaba sin
     * haber tocado nada y no se sabia si habia guardado algo o no. Ahora
     * aparece en cuanto cambia CUALQUIERA de las tres cosas (nombre de la red,
     * clave del punto de acceso o clave de administracion) y, si se cancela,
     * los campos vuelven a lo que hay guardado. */
    s_fila_botones = lv_obj_create(card1);
    lv_obj_remove_style_all(s_fila_botones);
    lv_obj_set_width(s_fila_botones, lv_pct(100));
    lv_obj_set_height(s_fila_botones, LV_SIZE_CONTENT);
    lv_obj_set_layout(s_fila_botones, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_fila_botones, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fila_botones, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_fila_botones, 8, 0);
    lv_obj_clear_flag(s_fila_botones, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_fila_botones, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_save = lv_btn_create(s_fila_botones);
    lv_obj_set_width(btn_save, lv_pct(62));
    lv_obj_set_height(btn_save, 40);
    lv_obj_set_style_bg_color(btn_save, UI_COLOR_GREEN, 0);
    lv_obj_add_event_cb(btn_save, wifi_save_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_save, LV_SYMBOL_OK "  Guardar");
    lv_obj_center(lbl_save);

    lv_obj_t *btn_cancel = lv_btn_create(s_fila_botones);
    lv_obj_set_width(btn_cancel, lv_pct(34));
    lv_obj_set_height(btn_cancel, 40);
    lv_obj_set_style_bg_color(btn_cancel, UI_COLOR_CARD_BORDER, 0);
    lv_obj_add_event_cb(btn_cancel, wifi_cancel_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_24_es, 0);
    lv_label_set_text(lbl_cancel, LV_SYMBOL_CLOSE "  Cancelar");
    lv_obj_center(lbl_cancel);

    /* Respuesta del boton. Sin esto lo pulsabas, se guardaba, se reiniciaba el
     * AP... y en pantalla no pasaba nada visible: la unica constancia quedaba en
     * el log, que el usuario no ve. */
    s_wifi_estado = lv_label_create(card1);
    lv_label_set_text(s_wifi_estado, "");
    lv_obj_set_style_text_font(s_wifi_estado, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_GREEN, 0);
    lv_obj_set_width(s_wifi_estado, lv_pct(100));
    lv_label_set_long_mode(s_wifi_estado, LV_LABEL_LONG_WRAP);


    /* === Card 2: Pagina inicial del portal + Reactivar (mitad ancho, dcho) ===
     * El desplegable de pagina inicial y, JUSTO DEBAJO, el boton para
     * reactivar el portal web (antes era una card independiente a lo ancho). */
    lv_obj_t *card2 = lv_obj_create(cont);
    lv_obj_set_width(card2, lv_pct(100));  /* el portal, a lo ancho, debajo */
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card2, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, UI_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(card2, 2, 0);
    lv_obj_set_style_radius(card2, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card2, 12, 0);
    lv_obj_set_style_pad_gap(card2, 8, 0);
    lv_obj_set_layout(card2, LV_LAYOUT_FLEX);
    /* DOS COLUMNAS (22-sep-2026, idea del usuario): el desplegable de la pagina
     * inicial a la izquierda y el portal web con su boton a la derecha. Ademas de
     * gustarle mas, resuelve el problema de raiz: al ir lado a lado no compiten por
     * el alto y no pueden pisarse (era lo que se veia como "DashPortalweb"). */
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_ROW);
    /* Reparte las dos filas en vertical: desplegable arriba, Reactivar abajo,
     * de modo que llenen la card (misma altura que "Punto de acceso"). */
    /* START + separacion fija en vez de SPACE_BETWEEN (22-sep-2026). Con
     * SPACE_BETWEEN, si la tarjeta se queda mas corta que su contenido, LVGL
     * apila los hijos unos encima de otros: el sintoma era el desplegable
     * pisando el texto. Con START y un hueco fijo eso no puede pasar. */
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* v3.10: la tarjeta pasa a columna; las dos columnas de siempre van a una
     * fila-cuerpo debajo de la cabecera centrada. */
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *card2_body = lv_obj_create(card2);
    lv_obj_remove_style_all(card2_body);
    lv_obj_set_size(card2_body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card2_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *card2_hdr = lv_label_create(card2);
    lv_obj_set_style_text_font(card2_hdr, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_hdr, UI_COLOR_GREEN, 0);
    lv_label_set_text(card2_hdr, LV_SYMBOL_LIST "  Portal web");
    ui_card_wrap_title(card2, card2_hdr, UI_COLOR_GREEN);

    /* Fila 1: desplegable de pagina inicial (el titulo ya esta en la cabecera) */
    lv_obj_t *card2_row1 = lv_obj_create(card2_body);
    lv_obj_remove_style_all(card2_row1);
    lv_obj_set_width(card2_row1, lv_pct(62));   /* izquierda: titulo + desplegable */
    lv_obj_set_height(card2_row1, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row1, LV_LAYOUT_FLEX);
    /* COLUMN: el desplegable baja a la linea de debajo del titulo para que no
     * se corte en la card estrecha (pct 49). */
    lv_obj_set_flex_flow(card2_row1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card2_row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card2_row1, 8, 0);

    /* Rotulo del desplegable (el titulo de la tarjeta ya esta en la cabecera). */
    lv_obj_t *card2_title = lv_label_create(card2_row1);
    lv_obj_set_style_text_font(card2_title, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(card2_title, UI_COLOR_TEXT_SOFT, 0);
    lv_label_set_text(card2_title, "Página inicial del portal");

    /* Dropdown: 0=Keys, 1=Logs, 2=Dashboard */
    lv_obj_t *dd_portal = ui_dd_create(card2_row1);
    lv_obj_set_width(dd_portal, lv_pct(75));   /* mas estrecho (lo pidio el usuario) */
    lv_dropdown_set_options(dd_portal, "Keys\nLogs\nDashboard");
    lv_obj_set_style_text_font(dd_portal, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd_portal), &lv_font_montserrat_24_es, 0);
    {
        nvs_handle_t h;
        uint8_t v = 2; /* default: Dashboard */
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "portal_page", &v);
            nvs_close(h);
        }
        if (v > 2) v = 2;
        lv_dropdown_set_selected(dd_portal, v);
    }
    lv_obj_add_event_cb(dd_portal, portal_page_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Fila 2: Portal web + boton Reactivar, justo debajo del desplegable.
     * El servidor HTTP se apaga solo tras 15 min sin nuevas asociaciones
     * (auto-off por seguridad). Este boton lo arranca de nuevo sin tener
     * que reasociar el movil ni reiniciar el display. */
    lv_obj_t *card2_row2 = lv_obj_create(card2_body);
    lv_obj_remove_style_all(card2_row2);
    lv_obj_set_width(card2_row2, lv_pct(36));   /* derecha: portal web + Reactivar */
    lv_obj_set_height(card2_row2, LV_SIZE_CONTENT);
    lv_obj_set_layout(card2_row2, LV_LAYOUT_FLEX);
    /* LADO A LADO, no apilados: la sonda de solapamientos midio que la etiqueta
     * 'Portal web' y el boton 'Reactivar' caian en el mismo rectangulo
     * (etiqueta 734,380-893,408 sobre boton 637,380-989,419). */
    lv_obj_set_flex_flow(card2_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_align(card2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *card2_react_title = lv_label_create(card2_row2);
    lv_obj_set_style_text_font(card2_react_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(card2_react_title, UI_COLOR_CYAN, 0);
    lv_label_set_text(card2_react_title, LV_SYMBOL_REFRESH "  Portal web");

    lv_obj_t *btn_react = lv_btn_create(card2_row2);
    lv_obj_set_height(btn_react, 40);
    lv_obj_set_width(btn_react, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn_react, 12, 0);   /* boton mas recogido (lo pidio el usuario) */
    lv_obj_set_style_radius(btn_react, 8, 0);
    lv_obj_set_style_bg_color(btn_react, UI_COLOR_CYAN, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn_react);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(btn_lbl, UI_COLOR_BG, 0);
    lv_label_set_text(btn_lbl, "Reactivar");
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn_react, reactivate_portal_cb, LV_EVENT_CLICKED, NULL);

    /* ── Card 4: credenciales del portal web (solo lectura) ──────────────
     * La pass HTTP es aleatoria (no derivable de la MAC). Se muestra aqui
     * para que el dueno pueda entrar a la web; solo visible en la pantalla
     * fisica del display. */
    char web_user[33] = {0};
    char web_pass[33] = {0};
    config_server_get_web_credentials(web_user, sizeof(web_user),
                                      web_pass, sizeof(web_pass));

    lv_obj_t *card4 = lv_obj_create(cont);
    lv_obj_set_width(card4, lv_pct(49));   /* el ACCESO va arriba, junto al AP */
    lv_obj_set_height(card4, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card4, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card4, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card4, UI_COLOR_GREEN, 0);
    lv_obj_set_style_border_width(card4, 2, 0);
    lv_obj_set_style_radius(card4, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card4, 12, 0);
    lv_obj_set_style_pad_gap(card4, 8, 0);
    lv_obj_set_layout(card4, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card4, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *c4_title = lv_label_create(card4);
    lv_obj_set_style_text_font(c4_title, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(c4_title, UI_COLOR_CYAN, 0);
    /* DOS CLAVES Y NO TRES (4-nov-2026, decision del dueno):
     *   1) la del PUNTO DE ACCESO, que es tambien la del portal (la que llevan
     *      la app del movil y la pantalla de cabina para hablar con la P4);
     *   2) la de ADMINISTRACION, solo para lo que puede dejar la placa
     *      inservible: grabar firmware (/ota), las claves AES de los Victron
     *      (/keys) y /save.
     * Antes habia una tercera pareja (usuario+clave del portal) y era la que se
     * veia aqui. Ahora la clave del portal ES la del Wi-Fi, y se cambia en la
     * tarjeta de arriba ("Punto de acceso"), que es donde se ve.
     *
     * LAS DESCARGAS VAN CON LA CLAVE DEL PORTAL, no con esta: el dueño lo dejo
     * claro ("cualquier descarga protegida por la clave del portal"). */
    lv_label_set_text(c4_title, LV_SYMBOL_SETTINGS "  Clave de administracion");
    ui_card_wrap_title(card4, c4_title, UI_COLOR_CYAN);

    char adm_user[33] = {0}, adm_pass[33] = {0};
    config_server_get_admin_credentials(adm_user, sizeof(adm_user),
                                        adm_pass, sizeof(adm_pass));
    lv_obj_t *c4_row = lv_obj_create(card4);
    lv_obj_remove_style_all(c4_row);
    lv_obj_set_width(c4_row, lv_pct(100));
    lv_obj_set_height(c4_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(c4_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c4_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c4_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c4_row, 8, 0);
    lv_obj_clear_flag(c4_row, LV_OBJ_FLAG_SCROLLABLE);

    s_c4_adm_lbl = lv_label_create(c4_row);
    lv_obj_set_style_text_font(s_c4_adm_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(s_c4_adm_lbl, "Clave:");

    /* LA CLAVE SE PUEDE ESCRIBIR, no solo generar (5-nov-2026, peticion del
     * dueño): asi se pone una que se recuerde y, sobre todo, el cambio se
     * puede GUARDAR o CANCELAR como el resto, en vez de aplicarse al instante
     * sin vuelta atras. */
    /* EL OJITO, para poder LEERLA (5-nov-2026). Antes esta clave se veia escrita
     * porque era una etiqueta; al convertirla en campo se quedo en puntitos y
     * ya no habia forma de leerla sin cambiarla. */
    s_adm_ojito = lv_btn_create(c4_row);
    lv_obj_set_size(s_adm_ojito, 50, 40);
    lv_obj_add_event_cb(s_adm_ojito, adm_ojito_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c4_ojito_lbl = lv_label_create(s_adm_ojito);
    lv_label_set_text(c4_ojito_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(c4_ojito_lbl);

    s_adm_ta = lv_textarea_create(c4_row);
    lv_obj_set_flex_grow(s_adm_ta, 1);
    lv_obj_set_height(s_adm_ta, 40);
    lv_textarea_set_one_line(s_adm_ta, true);
    lv_textarea_set_max_length(s_adm_ta, 32);
    lv_textarea_set_password_mode(s_adm_ta, true);
    lv_textarea_set_text(s_adm_ta, adm_pass);
    lv_obj_set_style_text_font(s_adm_ta, &lv_font_montserrat_20_es, 0);
    lv_obj_add_event_cb(s_adm_ta, ta_event_cb, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(s_adm_ta, ta_event_cb, LV_EVENT_DEFOCUSED, ui);
    lv_obj_add_event_cb(s_adm_ta, ta_event_cb, LV_EVENT_CANCEL, ui);
    lv_obj_add_event_cb(s_adm_ta, ta_event_cb, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(s_adm_ta, campo_cambiado_cb, LV_EVENT_VALUE_CHANGED, ui);

    lv_obj_t *c4_btn_admin = lv_btn_create(card4);
    lv_obj_set_width(c4_btn_admin, lv_pct(100));
    lv_obj_set_style_bg_color(c4_btn_admin, UI_COLOR_CYAN, 0);
    lv_obj_t *c4_lbl_admin = lv_label_create(c4_btn_admin);
    lv_obj_set_style_text_font(c4_lbl_admin, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(c4_lbl_admin, UI_COLOR_BG, 0);
    lv_label_set_text(c4_lbl_admin, LV_SYMBOL_SETTINGS "  Cambiar la clave");
    lv_obj_center(c4_lbl_admin);
    lv_obj_add_event_cb(c4_btn_admin, generar_clave_admin_cb, LV_EVENT_CLICKED, NULL);

    /* Igualar la altura de la card de ACCESO (la que va al lado) a la de "Punto de
     * acceso" (la mas alta) para que ambas queden simetricas lado a lado. */
    /* INTERCAMBIO pedido por el usuario (22-sep-2026): arriba, junto al punto de
     * acceso, va la tarjeta de ACCESO (usuario y clave, que es lo que hace
     * falta para entrar desde el movil); la pagina inicial del portal queda
     * debajo a ancho completo. Es solo el orden del hijo en el contenedor
     * (flex ROW_WRAP): card1 + card4 arriba, card2 debajo. */
    lv_obj_move_to_index(card4, 1);

    lv_obj_update_layout(cont);
    s_card1 = card1;
    s_card4 = card4;
    lv_coord_t h_ap = lv_obj_get_height(card1);
    if (h_ap > lv_obj_get_height(card4)) {
        lv_obj_set_height(card4, h_ap);
    }
}

/* Genera y aplica la clave de ADMINISTRACION. A partir de ahi, /ota, /keys y
 * /save solo la aceptan a ella. */
static void clave_admin_nueva_do(void)
{
    static const char abc[] = "abcdefghijkmnpqrstuvwxyz23456789";
    char user[33] = {0}, pass[33] = {0}, nueva[17] = {0};
    if (!s_adm_ta) return;
    config_server_get_admin_credentials(user, sizeof(user), pass, sizeof(pass));
    if (!user[0]) snprintf(user, sizeof(user), "portal");
    for (int i = 0; i < 12; i++) nueva[i] = abc[esp_random() % (sizeof(abc) - 1)];
    lv_textarea_set_password_mode(s_adm_ta, false);
    lv_textarea_set_text(s_adm_ta, nueva);
    if (s_adm_ojito) {
        lv_obj_t *lbl = lv_obj_get_child(s_adm_ojito, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_EYE_CLOSE);
    }
    refresca_botones();
    /* TEXTO CORTO A PROPOSITO (auditoria 13-nov): este dialogo mide 280 px y
     * caben 5 lineas de 24 px; antes pedia 10 y los botones tapaban justo las dos ultimas,
     * que eran "Mientras no guardes..." y "Apunta la clave AHORA". O sea que la
     * unica vez que se puede leer la clave nueva, no se leia entera. */
    char msg[520];
    snprintf(msg, sizeof(msg),
             "Usuario:  %s\nClave ADMIN:  %s\n\n"
             "Todavia NO esta guardada: pulsa GUARDAR.\n"
             "Apunta la clave AHORA.", user, nueva);
    ui_show_info_dialog(LV_SYMBOL_SETTINGS "  Clave nueva preparada", msg);
}

static void generar_clave_admin_cb(lv_event_t *e)
{
    (void)e;
    ui_show_confirm_dialog(LV_SYMBOL_WARNING "  Clave de administracion",
        "Se genera una clave APARTE para grabar\n"
        "firmware (/ota), las claves de los Victron\n"
        "y /save.\n\n"
        "La app y las pantallas siguen igual: no hay\n"
        "que tocar nada en ellas.",
        "Generar", clave_admin_nueva_do);
}

static void reactivate_portal_cb(lv_event_t *e)
{
    (void)e;
    /* Encolar, NO llamar a config_server_start() aqui: monta SPIFFS y lee NVS,
     * y estamos en el hilo de LVGL. El arranque es idempotente, asi que si el
     * portal ya estaba arriba el trabajo no hace nada. */
    config_server_request_start();
    ESP_LOGI("settings_panel", "Reactivar portal web: solicitado");
}

/* Guarda SSID y password y reaplica el AP en caliente. Sustituye al guardado
 * automatico al perder el foco, que renombraba el AP sin pedirlo. */
/* ¿Hay algún cambio sin guardar? Se comparan los TRES campos con lo que hay
 * guardado: el nombre de la red, la clave del punto de acceso y la clave de
 * administración. */
static bool hay_cambios(void)
{
    if (s_ssid_ta && strcmp(lv_textarea_get_text(s_ssid_ta), s_ssid_guardado) != 0) return true;
    if (s_pass_ta && strcmp(lv_textarea_get_text(s_pass_ta), s_pass_guardada) != 0) return true;
    if (s_adm_ta && strcmp(lv_textarea_get_text(s_adm_ta), s_adm_guardada) != 0) return true;
    return false;
}

/* Enseña la fila Guardar/Cancelar SOLO si hay algo cambiado. */
static void refresca_botones(void)
{
    if (!s_fila_botones) return;
    bool hay = hay_cambios();
    if (hay) lv_obj_clear_flag(s_fila_botones, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(s_fila_botones, LV_OBJ_FLAG_HIDDEN);
    /* Y que se lea en la linea de estado, no solo por los botones: si dices
     * "no hay nada que guardar" mientras hay cambios, el mensaje miente. Y al
     * reves: si el aviso se queda puesto cuando ya no hay cambios (teclear una
     * letra y borrarla), tambien miente. Por eso solo se toca ese texto: los
     * mensajes de "Guardado." o de error se respetan. */
    /* Las dos tarjetas de arriba, a la misma altura: al salir la fila la
     * primera crece unos 40 px y la segunda se quedaba corta. */
    if (s_card1 && s_card4) {
        lv_obj_update_layout(s_card1);
        lv_obj_set_height(s_card4, lv_obj_get_height(s_card1));
    }
    if (s_wifi_estado) {
        const char *aviso = "Hay cambios sin guardar";
        if (hay) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_ORANGE, 0);
            lv_label_set_text(s_wifi_estado, aviso);
        } else if (strcmp(lv_label_get_text(s_wifi_estado), aviso) == 0) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_TEXT_DIM, 0);
            lv_label_set_text(s_wifi_estado, "");
        }
    }
}

static void campo_cambiado_cb(lv_event_t *e)
{
    (void)e;
    refresca_botones();
}

/* Enseñar/tapar la clave de administracion mientras se mira. No guarda nada. */
static void adm_ojito_cb(lv_event_t *e)
{
    if (!s_adm_ta) return;
    bool oculto = lv_textarea_get_password_mode(s_adm_ta);
    lv_textarea_set_password_mode(s_adm_ta, !oculto);
    lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target(e), 0);
    if (lbl) lv_label_set_text(lbl, oculto ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

/* Cancelar: los campos vuelven a lo que hay guardado y la fila se esconde. */
static void wifi_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_ssid_ta) lv_textarea_set_text(s_ssid_ta, s_ssid_guardado);
    if (s_pass_ta) lv_textarea_set_text(s_pass_ta, s_pass_guardada);
    if (s_adm_ta) lv_textarea_set_text(s_adm_ta, s_adm_guardada);
    /* Y TAPADA OTRA VEZ (5-nov-2026, auditoria): "Cambiar la clave" la deja a
     * la vista (y pone el ojo en "cerrar"); al cancelar se quedaba escrita en
     * la pantalla, que es justo lo que no puede pasar con esta clave. */
    if (s_adm_ta) lv_textarea_set_password_mode(s_adm_ta, true);
    if (s_adm_ojito) {
        lv_obj_t *lbl = lv_obj_get_child(s_adm_ojito, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_EYE_OPEN);
    }
    if (s_wifi_estado) {
        lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(s_wifi_estado, "Sin cambios: no se ha guardado nada");
    }
    refresca_botones();
}

static void wifi_save_cb(lv_event_t *e)
{
    ui_state_t *ui = (ui_state_t *)lv_event_get_user_data(e);
    if (ui == NULL || ui->wifi.ssid == NULL || ui->wifi.password == NULL) return;

    const char *ssid = lv_textarea_get_text(ui->wifi.ssid);
    const char *pass = lv_textarea_get_text(ui->wifi.password);

    /* Un SSID vacio dejaria el AP sin nombre: wifi_ap_init lo rechaza y vuelve
     * al de fabrica, asi que mejor no guardar nada y que se vea que no ha
     * pasado nada. */
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGW(TAG_SETTINGS, "SSID vacio: no se guarda");
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "El nombre de la red no puede estar vacio");
        }
        return;
    }
    /* Tope 802.11 (32 caracteres). lv_textarea_set_max_length ya lo impide
     * al teclear, pero esto es la validacion de verdad -- por si algun dia
     * el SSID llega por otro camino que no pase por ese textarea. */
    if (strlen(ssid) > 32) {
        ESP_LOGW(TAG_SETTINGS, "SSID de %u caracteres: no se guarda (max 32)",
                 (unsigned)strlen(ssid));
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "El nombre de la red no puede pasar de 32 caracteres");
        }
        return;
    }
    /* WPA2 exige 8-63 caracteres. wifi_ap_init() (config_server_ap.c) lee esta
     * clave de NVS SIN revalidarla -- confia en que config_server_ensure_ap_
     * password() ya la dejo buena, pero esa funcion solo corre UNA VEZ al
     * arrancar, antes de la UI. Sin este chequeo, guardar aqui una clave
     * corta (o en blanco) desde la pantalla la dejaba tal cual en NVS, y el
     * siguiente wifi_ap_init() la usaba igual -> AP con WPA2 mal configurado,
     * probablemente sin arrancar hasta reflashear por USB. Detectado
     * auditando el 08-sep-2026. */
    size_t pass_len = pass ? strlen(pass) : 0;
    if (pass_len < 8) {
        ESP_LOGW(TAG_SETTINGS, "clave WPA2 de %u caracteres: no se guarda", (unsigned)pass_len);
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "La clave Wi-Fi necesita al menos 8 caracteres (WPA2)");
        }
        return;
    }
    /* TOPE 32, NO 63 (5-nov-2026, auditoria). WPA2 admite 63, pero esta clave es
     * TAMBIEN la del portal abierto (usuario + clave) y ahi los buffers son de
     * 33: http_auth_init() lee "http_pass" en pass[33] (config_server_auth.c) y
     * config_server_set_web_credentials() rechaza mas de 32. Con una clave de
     * 33-63 el guardado decia "Guardado", el portal seguia con la vieja y, al
     * reiniciar, http_pass no cabia -> el portal entero contestaba 401. Mejor
     * no dejarla entrar desde aqui y decirlo claro. */
    if (pass_len > 32) {
        ESP_LOGW(TAG_SETTINGS, "clave del AP de %u caracteres: no se guarda (max 32)", (unsigned)pass_len);
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado,
                "La clave del punto de acceso no puede pasar de 32 caracteres "
                "(la web y la app no admiten mas)");
        }
        return;
    }

    /* LA CLAVE DE ADMINISTRACION SE MIRA ANTES DE ESCRIBIR NADA (5-nov-2026).
     * Antes se grababa primero la del Wi-Fi y la de administracion se validaba
     * al final: si esa fallaba, la del Wi-Fi ya estaba en la NVS y la fila
     * Guardar/Cancelar seguia pidiendo guardar. Con el aviso aqui, un Guardar
     * o guarda las DOS claves o no guarda ninguna. */
    const char *adm = s_adm_ta ? lv_textarea_get_text(s_adm_ta) : NULL;
    bool adm_cambiada = (adm != NULL && strcmp(adm, s_adm_guardada) != 0);
    if (adm_cambiada && strlen(adm) < 8) {
        ESP_LOGW(TAG_SETTINGS, "clave de administracion corta: no se guarda");
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado,
                "La clave de administracion necesita al menos 8 caracteres");
        }
        return;
    }

    /* Si cambia la RED (nombre o clave del punto de acceso) hay que decirlo:
     * la clave nueva NO entra hasta que la radio se reinicia (ver abajo), y
     * eso el usuario tiene que saberlo, no descubrirlo. Se mira por separado
     * porque el aviso de "clave nueva" no vale cuando solo cambia el nombre. */
    bool cambia_ssid  = (strcmp(ssid, s_ssid_guardado) != 0);
    bool cambia_clave = (strcmp(pass ? pass : "", s_pass_guardada) != 0);
    bool cambia_red   = cambia_ssid || cambia_clave;

    /* TODO EN UNA SOLA TRANSACCION (5-nov-2026, auditoria). Antes se grababa en
     * tres tandas: primero ssid/password, luego las credenciales del portal y
     * luego la de administracion. Si una de las dos ultimas fallaba, la de la
     * red ya estaba dentro: la fila seguia pidiendo guardar, Cancelar decia
     * "no se ha guardado nada" y al reiniciar aparecia la clave que el usuario
     * creia haber cancelado. Ahora se escriben TODAS las claves en el mismo
     * handle y hay UN commit: o entra todo o no entra nada. */
    char adm_user[33] = {0}, adm_ver[33] = {0};
    config_server_get_admin_credentials(adm_user, sizeof(adm_user),
                                        adm_ver, sizeof(adm_ver));   /* el usuario manda; la clave se reescribe */
    if (adm_user[0] == '\0') snprintf(adm_user, sizeof(adm_user), "portal");

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SETTINGS, "nvs_open failed: %s", esp_err_to_name(err));
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "No se pudo guardar: la memoria interna no responde");
        }
        return;
    }
    /* La clave del punto de acceso es TAMBIEN la del portal abierto (la de la
     * app y la cabina, con usuario "p4"): se graba en la misma transaccion. */
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "password", pass ? pass : "");
    if (err == ESP_OK) err = nvs_set_str(h, "http_user", "p4");
    if (err == ESP_OK) err = nvs_set_str(h, "http_pass", pass ? pass : "");
    if (adm_cambiada && err == ESP_OK) {
        err = nvs_set_str(h, "adm_user", adm_user);
        if (err == ESP_OK) err = nvs_set_str(h, "adm_pass", adm);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SETTINGS, "no he podido guardar las claves: %s", esp_err_to_name(err));
        if (s_wifi_estado) {
            lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_RED, 0);
            lv_label_set_text(s_wifi_estado, "No se pudo guardar: no ha cambiado nada");
        }
        return;
    }
    /* Y con lo grabado se rehacen las DOS cabeceras Basic de una vez. */
    http_auth_init();

    /* Lo guardado pasa a ser la referencia nueva: la fila se esconde sola. */
    snprintf(s_ssid_guardado, sizeof(s_ssid_guardado), "%s", ssid);
    snprintf(s_pass_guardada, sizeof(s_pass_guardada), "%s", pass ? pass : "");
    if (adm_cambiada) {
        snprintf(s_adm_guardada, sizeof(s_adm_guardada), "%s", adm);
        ESP_LOGW(TAG_SETTINGS, "clave de administracion cambiada desde la pantalla");
    }
    refresca_botones();

    ESP_LOGI(TAG_SETTINGS, "Claves guardadas: SSID='%s' cambia_ssid=%d cambia_clave=%d adm=%d",
             ssid, cambia_ssid, cambia_clave, (int)adm_cambiada);
    if (s_wifi_estado) {
        lv_obj_set_style_text_color(s_wifi_estado, UI_COLOR_GREEN, 0);
        if (cambia_clave && cambia_ssid) {
            lv_label_set_text(s_wifi_estado,
                "Guardado el nombre y la clave. La web, la app y la cabina ya piden "
                "la clave nueva; para entrar en el Wi-Fi, reinicia la red");
        } else if (cambia_clave) {
            lv_label_set_text(s_wifi_estado,
                "Guardado. La web, la app y la cabina ya piden la clave nueva; "
                "para entrar en el Wi-Fi, reinicia la red (interruptor o Reiniciar)");
        } else if (cambia_ssid) {
            lv_label_set_text(s_wifi_estado,
                "Guardado. El nombre nuevo de la red sale al reiniciarla "
                "(interruptor o Reiniciar)");
        } else {
            lv_label_set_text(s_wifi_estado, "Guardado.");
        }
    }
    /* SIN REAPLICAR EL AP EN CALIENTE (5-nov-2026). Aqui habia un
     * config_server_request_wifi_apply(). Medido en la placa: esa funcion, con
     * la radio YA ENCENDIDA, hace esp_wifi_set_config + esp_wifi_start otra vez
     * y con esp_hosted (la C6 por SDIO) eso deja el punto de acceso mudo -- se
     * sigue viendo la red desde el movil, pero la P4 ya no contesta ni a un
     * ping hasta reiniciar. Reproducido tres veces.
     *
     * El interruptor de la cabecera NO pasa por ahi: apaga la radio y la vuelve
     * a encender (parar y arrancar, que es el camino bueno) y ademas relee la
     * NVS, asi que es tambien la forma de que entre la clave nueva. Guardar
     * aqui solo escribe: las DOS claves quedan guardadas y el portal y la clave
     * de administracion se aplican en el acto (http_auth_init), que es lo que
     * hace falta para la app, las cabinas y las descargas. */
}

void password_toggle_btn_event_cb(lv_event_t *e)
{
    ui_state_t *ui = lv_event_get_user_data(e);
    if (ui == NULL || ui->wifi.password == NULL) {
        return;
    }

    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    bool new_mode = !lv_textarea_get_password_mode(ui->wifi.password);
    lv_textarea_set_password_mode(ui->wifi.password, new_mode);

    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_label_set_text(label, new_mode ? "Show" : "Hide");
    }
}

static void portal_page_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "portal_page", (uint8_t)sel);
        esp_err_t err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG_SETTINGS, "portal_page no persistio: %s", esp_err_to_name(err));
        nvs_close(h);
    }
    const char *name = sel == 0 ? "Keys" : (sel == 1 ? "Logs" : "Dashboard");
    ESP_LOGI(TAG_SETTINGS, "Portal page: %s", name);
}
