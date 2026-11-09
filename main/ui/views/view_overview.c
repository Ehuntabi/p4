#include "view_overview.h"
#include "ui/widgets/ui_card.h"
#include "data/energy_today.h"
#include "fonts/fonts_es.h"
#include "icons/icons.h"
#include "ui.h"
#include "alarma_estado.h"   /* estado compartido de las cuatro alarmas */
#include "ne185/ne185.h"
#include "frigo.h"
#include "audio_es8311.h"
#include "alerts.h"
#include "ui/widgets/lv_font_thermometer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ui_device_view_t base;
    /* Cards */
    lv_obj_t *card_solar;
    lv_obj_t *m_solar_w;
    lv_obj_t *pill_solar_state;   /* pastilla estado MPPT (Bulk/Float/...) */
    /* Card central Bateria */
    lv_obj_t *card_bat;
    lv_obj_t *arc_soc;
    lv_obj_t *m_bat_current;
    /* Card DC/DC (Orion-Tr Smart, carga desde batería motor) */
    lv_obj_t *card_loads;      /* nombre mantenido por compatibilidad */
    lv_obj_t *m_loads_w;       /* V_in: tensión batería motor */
    lv_obj_t *pill_dcdc_state; /* pastilla estado Orion (Bulk/Float/...) */
    /* Métricas extra dentro de cada card */
    lv_obj_t *m_ttg;          /* dentro de card_bat: Autonomía */
    lv_obj_t *m_solar_kwh;    /* dentro de card_solar: kWh hoy */
    lv_obj_t *m_loads_kwh;    /* dentro de card_dcdc: V_out (servicio) */

    /* State-store DC/DC */
    struct {
        bool     has_data;
        uint8_t  state;       /* device_state: 0=off, otros valores=activos */
        int16_t  vin_centi;   /* tension entrada (bateria motor) */
        int16_t  vout_centi;  /* tension salida (bateria servicio) */
        uint32_t last_update_ms;
    } dcdc;

    /* State-store consolidado por tipo */
    struct {
        bool has_data;
        uint16_t soc_deci;
        uint16_t voltage_centi;
        int32_t current_milli;
        uint32_t ttg_min;
        uint32_t last_update_ms;
    } bat;
    struct {
        bool has_data;
        uint8_t  state;       /* device_state: 0=off, otros=activos */
        uint16_t pv_w;
        int16_t  load_current_deci;
        uint16_t voltage_centi;
        uint32_t last_update_ms;
    } solar;

    /* ── Widgets camper (NE185 via UART) ─────────────────────── */
    lv_obj_t *tank_s1;           /* tanque agua limpia (debajo de Solar) */
    lv_obj_t *tank_r1;           /* tanque aguas grises (debajo de DC/DC) */
    lv_obj_t *pill_shore;        /* indicador 230 V (debajo de Bateria) */
    lv_obj_t *pill_shore_lbl;    /* texto interno del pill (ON/OFF) */
    /* ── Widgets frigo (DS18B20 + ventilador PWM) ─────────────── */
    lv_obj_t *lbl_freezer_temp;  /* T_Congelador valor numerico (fuente grande) */
    lv_obj_t *ln_freezer_trend;  /* flecha de tendencia (dibujada con líneas) */
    lv_obj_t *lbl_freezer_unit;  /* unidad "C" en fuente _es (tiene glifo grado) */
    lv_obj_t *img_fan;           /* icono ventilador (animado, debajo 230V) */
    lv_obj_t *arc_fan;           /* aro-gauge del PWM 0..100 % alrededor del ventilador */
    int       fan_angle_deci;    /* angulo actual rotacion (0..3599) */
    /* ── Icono del altavoz de cada alarma ──────────────────────────────
     * Uno por tarjeta que puede estar en alarma, oculto mientras su alarma no
     * este activa. Ver alarm_mute_icono_cb() y refresh_mute_iconos().
     * El ESTADO no vive aqui: vive en alarma_estado.c, que es lo que comparten
     * la pantalla, la telemetria UDP y la orden que llega de la cabina. */
    lv_obj_t *ic_mute_s1;
    lv_obj_t *ic_mute_r1;
    lv_obj_t *ic_mute_bat;
    lv_obj_t *ic_mute_frigo;
    uint8_t   blink_phase;           /* alterna 0/1 cada tick para parpadeo */
    lv_obj_t *camper_bottom;     /* contenedor de los 3 botones */
    lv_obj_t *btn_lin;
    lv_obj_t *btn_lout;
    lv_obj_t *btn_pump;
    bool      grey_aligned;      /* one-shot: ancho 230V + base alineada a limpias */
    lv_timer_t *camper_tick_timer; /* refresco periodico widgets camper */
    lv_timer_t *fan_rotate_timer;  /* animacion rotacion ventilador */
    /* Aviso de estado (Solar / DC-DC) a pantalla completa. Vive en
     * lv_layer_top() para verse desde cualquier pantalla, asi que NO cuelga del
     * root de la vista: si no se borra a mano al destruirla, se queda flotando
     * encima. Se guarda aqui para poder borrarlo. */
    lv_obj_t *info_modal;
} ui_overview_view_t;

static void overview_update(ui_device_view_t *view, const victron_data_t *data);
static void overview_show(ui_device_view_t *view);
static void overview_hide(ui_device_view_t *view);
static void overview_destroy(ui_device_view_t *view);
static void overview_render(ui_overview_view_t *ov);

/* Ultimo nivel pintado en cada tanque (O1). 0xFE = "no pintado aun", para que
 * la primera vuelta pinte siempre. Se resetea en ui_overview_view_create: si
 * algun dia la vista se recrea, los widgets son nuevos y el cache no vale. */
static uint8_t s_ov_last_s1 = 0xFE;
static uint8_t s_ov_last_r1 = 0xFE;
static uint32_t now_ms(void) { return lv_tick_get(); }

/* ── Estado Victron (device_state): texto, color y explicacion ──────
 * Mismo criterio de color que la pantalla de detalle del MPPT. Se usa en las
 * pastillas de estado de las cards Solar (MPPT) y DC/DC (Orion). */
static const char *ov_state_name(uint8_t s)
{
    switch (s) {
    case 0:  return "Apagado";
    case 1:  return "Bajo consumo";
    case 2:  return "Fallo";
    case 3:  return "Bulk";
    case 4:  return "Absorcion";
    case 5:  return "Float";
    case 6:  return "Storage";
    case 7:  return "Eq. manual";
    case 8:  return "Eq. auto";
    case 9:  return "Inversor";
    case 10: return "Fuente";
    case 11: return "Iniciando";
    default: return "-";
    }
}

static lv_color_t ov_state_color(uint8_t s)
{
    switch (s) {
    case 3:  return UI_COLOR_ORANGE;   /* Bulk */
    case 4:  return UI_COLOR_YELLOW;   /* Absorcion */
    case 5:                            /* Float */
    case 6:  return UI_COLOR_GREEN;    /* Storage */
    case 2:  return UI_COLOR_RED;      /* Fallo */
    default: return UI_COLOR_TEXT_DIM;
    }
}

static const char *ov_state_help(uint8_t s)
{
    switch (s) {
    case 0:  return "En reposo: no esta cargando ni entregando energia.";
    case 1:  return "Modo de bajo consumo (reposo).";
    case 2:  return "Hay un fallo. Conviene revisar el equipo.";
    case 3:  return "Carga rapida (Bulk): mete toda la corriente disponible "
                    "para subir la bateria lo antes posible.";
    case 4:  return "Absorcion: la bateria esta casi llena; mantiene la "
                    "tension para terminar de cargarla sin forzarla.";
    case 5:  return "Float: la bateria esta llena; solo la mantiene a flote "
                    "sin sobrecargar.";
    case 6:  return "Storage: bateria llena y en reposo, con una carga minima "
                    "de mantenimiento.";
    case 7:  return "Ecualizacion manual: carga alta puntual para equilibrar "
                    "las celdas de la bateria.";
    case 8:  return "Ecualizacion automatica: carga alta puntual para "
                    "equilibrar las celdas de la bateria.";
    case 9:  return "Generando 230 V desde la bateria (inversor).";
    case 10: return "Modo fuente de alimentacion (tension fija).";
    case 11: return "Arrancando...";
    default: return "Estado no disponible.";
    }
}

/* Cierra (async) el modal de info; la vista va como user_data. */
static void ov_info_modal_close_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    if (!ov || !ov->info_modal) return;
    lv_obj_del_async(ov->info_modal);
    ov->info_modal = NULL;
}

/* Aviso a pantalla completa explicando en cristiano el estado actual. */
static void ov_show_state_info(ui_overview_view_t *ov, uint8_t state)
{
    if (!ov) return;
    /* Si ya habia uno abierto, fuera: asi no se apilan */
    if (ov->info_modal) { lv_obj_del(ov->info_modal); ov->info_modal = NULL; }
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    ov->info_modal = modal;
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, ov_info_modal_close_cb, LV_EVENT_CLICKED, ov);

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, ov_state_color(state), 0);
    lv_obj_set_style_border_width(dlg, 3, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, ov_state_color(state), 0);
    lv_label_set_text(title, ov_state_name(state));

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, ov_state_help(state));

    lv_obj_t *btn = lv_btn_create(dlg);
    lv_obj_set_style_bg_color(btn, ov_state_color(state), 0);
    lv_obj_add_event_cb(btn, ov_info_modal_close_cb, LV_EVENT_CLICKED, ov);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20_es, 0);
    lv_label_set_text(btn_lbl, "Entendido");
    lv_obj_center(btn_lbl);
}

static void ov_solar_state_pill_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = lv_event_get_user_data(e);
    if (ov) ov_show_state_info(ov, ov->solar.state);
}
static void ov_dcdc_state_pill_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = lv_event_get_user_data(e);
    if (ov) ov_show_state_info(ov, ov->dcdc.state);
}

/* Pulsar el cuerpo de una card abre su detalle a pantalla completa. Los pills
 * de estado conservan su propio handler (los clicks no burbujean en LVGL v8). */
static void ov_card_solar_clicked(lv_event_t *e)
{
    ui_overview_view_t *ov = lv_event_get_user_data(e);
    if (ov) ui_show_card_detail(ov->base.ui, VICTRON_BLE_RECORD_SOLAR_CHARGER);
}
static void ov_card_dcdc_clicked(lv_event_t *e)
{
    ui_overview_view_t *ov = lv_event_get_user_data(e);
    if (ov) ui_show_card_detail(ov->base.ui, VICTRON_BLE_RECORD_DCDC_CONVERTER);
}

/* Robustez del toque: los contenedores hijos de una card (header, filas,
 * metricas) se crean con lv_obj_create y capturan el toque -o lo interpretan
 * como scroll- antes de que llegue a la card. Recorremos su arbol y hacemos que
 * los hijos NO scrolleen y BURBUJEEN el evento hacia la card, para que un toque
 * en cualquier parte abra el detalle. `except` (el pill de estado) se excluye
 * para que conserve su propio handler. */
static void ov_card_make_tappable(lv_obj_t *obj, lv_obj_t *except)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(obj, i);
        if (ch == except) continue;
        lv_obj_clear_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ch, LV_OBJ_FLAG_EVENT_BUBBLE);
        ov_card_make_tappable(ch, except);
    }
}

/* Pastilla de estado pequena (fuente 20 + poco relleno), pulsable. Se crea
 * como ultimo hijo de la card para que quede al fondo, bajo los valores. */
static lv_obj_t *ov_make_state_pill(lv_obj_t *card)
{
    lv_obj_t *pill = ui_pill_create(card, "-", UI_COLOR_TEXT_DIM);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(pill, 12, 0);
    lv_obj_set_style_pad_ver(pill, 4, 0);
    lv_obj_t *lbl = lv_obj_get_child(pill, 0);
    if (lbl) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20_es, 0);
    return pill;
}

/* ── ESTÉTICA DE TARJETA CON DATO (banco de pruebas, 06-nov-2026) ───────────
 *
 * El dueño pidió a la tarjeta Solar una vuelta "profesional" sin mover nada de
 * sitio, y al verla quiso lo mismo en la del DC/DC. Esto es lo que se aplica,
 * en un solo sitio para que las dos (y las que vengan) sean idénticas:
 *
 *   FONDO Y MARCO
 *     - superficie un punto más clara que el fondo (UI_COLOR_CARD_HI) para que
 *       la tarjeta se despegue sin necesidad de un marco gordo. Se probó un
 *       degradado suave y la pantalla, que es de 16 bits, lo sacaba a bandas:
 *       descartado por verse como un defecto.
 *     - marco de 1 px (antes 2) al 80 %: el color de la tarjeta sigue viéndose
 *       en el filo, pero deja de competir con el icono y con la cifra.
 *     - esquina 12 en vez de 16: menos "burbuja", más instrumento.
 *     - sombra algo más abierta y desplazada, para dar profundidad.
 *   CABECERA
 *     - el título deja de ser un titular de 28 px del color de la tarjeta y
 *       pasa a etiqueta pequeña, en mayúsculas, gris y espaciada: el color se
 *       queda en el icono y en el filo.
 *   DATOS
 *     - rótulo a la izquierda, gris y a 20; cifra a la derecha a 46; unidad
 *       pequeña y gris, al ras de la cifra; un filete de 1 px entre los dos
 *       datos.
 *     - la cifra y la unidad llevan ANCHO FIJO a propósito: con ancho
 *       automático, cuando el texto crece (de "--" a "12.4") la caja del padre
 *       no se re-mide y el número sale recortado por la izquierda. Con ancho
 *       fijo las dos cifras acaban en la misma vertical aunque cambie el largo.
 */
#define OV_CIFRA_W   96     /* ancho fijo de la cifra (aguanta "12.4") */
#define OV_UNIDAD_W  42     /* ancho fijo de la unidad ("kWh" mide 37) */

static void ov_estilo_marco(lv_obj_t *card, lv_color_t accent)
{
    if (!card) return;
    /* FONDO CON EL TINTE DE SU MARCO, MUY SUTIL (09-nov-2026): el mismo token
     * que las tarjetas de las vistas (UI_CARD_TINTE_MIX, 4 %), sobre la
     * superficie clara de esta pantalla (UI_COLOR_CARD_HI). Asi las cinco
     * tarjetas de la pantalla de inicio -- verde/ambar/cian arriba,
     * violeta/hielo abajo -- llevan cada una su tono sin que se lea como un
     * semaforo, y coinciden con el filo. */
    lv_obj_set_style_bg_color(card, ui_card_superficie(accent, UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_shadow_ofs_y(card, 4, 0);
}

static void ov_estilo_cabecera(lv_obj_t *card, const char *titulo)
{
    if (!card) return;
    lv_obj_t *header = lv_obj_get_child(card, 0);
    lv_obj_t *left   = header ? lv_obj_get_child(header, 0) : NULL;
    lv_obj_t *tit    = left ? lv_obj_get_child(left, 1) : NULL;
    if (tit) {
        if (titulo) lv_label_set_text(tit, titulo);
        lv_obj_set_style_text_font(tit, UI_FONT_TEXT, 0);
        /* BLANCO, que destaque (06-nov-2026, pedido del dueño): antes iba en
         * gris como el resto de rótulos y el título de la tarjeta se perdía. */
        lv_obj_set_style_text_color(tit, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_letter_space(tit, UI_LS_TITULO, 0);
    }
    if (header) lv_obj_set_style_border_width(header, 1, 0);
}

static void ov_estilo_tarjeta(lv_obj_t *card, lv_color_t accent,
                              const char *titulo,
                              lv_obj_t *m1, lv_obj_t *m2)
{
    if (!card) return;
    ov_estilo_marco(card, accent);
    ov_estilo_cabecera(card, titulo);

    lv_obj_t *mets[2] = { m1, m2 };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *box = mets[i];
        if (!box) continue;
        /* La caja medía lo que su contenido: sin ancho completo no hay
         * "izquierda/derecha" que repartir. */
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        /* Poco aire arriba/abajo del dato: con mucho, el bloque empujaba la
         * pastilla de estado contra el filo de abajo de la tarjeta. 8 (era 6,
         * fuera de la rejilla) sigue siendo poco: +2 px y el bloque no llega a
         * tocar la pastilla. */
        lv_obj_set_style_pad_ver(box, 8, 0);
        lv_obj_t *lab = lv_obj_get_child(box, 0);
        lv_obj_t *row = lv_obj_get_child(box, 1);
        lv_obj_t *val = row ? lv_obj_get_child(row, 0) : NULL;
        lv_obj_t *uni = row ? lv_obj_get_child(row, 1) : NULL;
        if (lab) {
            lv_obj_set_style_text_font(lab, UI_FONT_TEXT, 0);
            lv_obj_set_style_text_color(lab, UI_COLOR_TEXT_DIM, 0);
            /* Alinear abajo pone los dos textos al ras por el borde de su caja,
             * no por la línea base: el rótulo queda alto. Este relleno lo baja
             * a la altura de la cifra. */
            lv_obj_set_style_pad_bottom(lab, 8, 0);
        }
        if (val) {
            lv_obj_set_style_text_font(val, UI_FONT_DISPLAY, 0);
            lv_obj_set_width(val, OV_CIFRA_W);
            lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        }
        if (uni) {
            lv_obj_set_style_text_font(uni, UI_FONT_TEXT, 0);
            lv_obj_set_style_text_color(uni, UI_COLOR_TEXT_DIM, 0);
            lv_obj_set_style_pad_bottom(uni, 8, 0);
            lv_obj_set_width(uni, OV_UNIDAD_W);
            lv_obj_set_style_text_align(uni, LV_TEXT_ALIGN_LEFT, 0);
            /* Con ancho fijo "kWh" partiría en dos líneas: que recorte antes
             * que partir. */
            lv_label_set_long_mode(uni, LV_LABEL_LONG_CLIP);
        }
        if (i == 0) {   /* filete entre los dos datos */
            lv_obj_set_style_border_color(box, UI_COLOR_CARD_BORDER, 0);
            lv_obj_set_style_border_side(box, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_opa(box, LV_OPA_20, 0);
        }
    }
}

static lv_obj_t *create_node_card(lv_obj_t *parent, const lv_img_dsc_t *img,
                                  const char *title, lv_color_t accent,
                                  lv_obj_t **out_metric)
{
    lv_obj_t *card = ui_card_create(parent, accent);
    /* Header con icono raster + metric debajo. El caller fija altura/grow */
    ui_card_set_title_img(card, img, title, accent);
    if (out_metric) {
        *out_metric = ui_metric_create_compact(card, "");
    }
    return card;
}

/* ───────────────────────────────────────────────────────────────
 * Widgets camper (NE185): barra superior con tanques + 230V,
 * fila inferior con 3 botones (luz int, luz ext, bomba).
 * ─────────────────────────────────────────────────────────────── */
/* ── Alarmas ────────────────────────────────────────────────────────────────
 * El ESTADO (condicion activa, silencio, temporizaciones y pitido) ya NO vive
 * aqui: se mudo a main/alarma_estado.c el 14-sep-2026, cuando la cabina 3,5"
 * paso a poder silenciarlas y la telemetria UDP tuvo que publicar cuales estan
 * activas. Esta vista es ahora UN consumidor mas: pinta el parpadeo y
 * el icono del altavoz, y llama a alarma_silenciar()/alarma_alternar_silencio()
 * cuando se toca. Ver el porque en la cabecera de alarma_estado.h. El aviso
 * flotante y la interrupcion del salvapantallas viven en alarma_estado.c (su
 * timer corre aunque esta vista este oculta). */

/* ── Icono del altavoz, en la esquina de la tarjeta en alarma ───────────────
 * Mismo gesto y mismo dibujo que en la cabina 3,5" (pedido del usuario,
 * 14-sep-2026: que las dos pantallas se vean y se comporten igual):
 *   - solo se ve mientras ESA alarma esta activa (no es un boton permanente);
 *   - la zona tactil es mayor que el dibujo (lv_obj_set_ext_click_area), que es
 *     lo que hace falta con el dedo y en marcha;
 *   - un toque SILENCIA, el siguiente vuelve a habilitar el sonido.
 *
 * EL BOTON CANCELA LA PROPAGACION del evento. Sin eso el toque llegaria tambien
 * a la tarjeta, que abre su pantalla de detalle (o cambia el brillo, que
 * escucha en la pantalla entera): tocar el altavoz acabaria abriendo otra
 * pantalla, que no es lo que se ha pedido.
 *
 * El numero de zona tactil es 14, el mismo que usan las flechas del historico. */
#define MUTE_CLICK_EXTRA 14

static void alarm_mute_icono_cb(lv_event_t *e)
{
    alarma_tipo_t t = (alarma_tipo_t)(intptr_t)lv_event_get_user_data(e);
    lv_event_stop_bubbling(e);
    alarma_alternar_silencio(t);
}

static lv_obj_t *alarm_mute_icono_crear(lv_obj_t *card, alarma_tipo_t t)
{
    lv_obj_t *ic = lv_label_create(card);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_label_set_text(ic, "");
    /* Fuente _es: es la que trae los glifos de FontAwesome. Comprobado que
     * LV_SYMBOL_MUTE (U+F026) esta en la tabla de caracteres de esta fuente; si
     * algun dia no estuviera, aqui saldria un rectangulo (ya paso con el punto
     * medio en esta pantalla). */
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20_es, 0);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ic, MUTE_CLICK_EXTRA);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);   /* hasta que su alarma este activa */
    lv_obj_align(ic, LV_ALIGN_TOP_RIGHT, 2, 2);
    lv_obj_add_event_cb(ic, alarm_mute_icono_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t);
    return ic;
}

/* Pinta los cuatro iconos segun el estado compartido. Se agrupa en una funcion
 * para que la tarjeta y el icono digan SIEMPRE lo mismo (salir por un camino
 * que olvide actualizar el icono dejaria una pantalla mintiendo). */
static void refresh_mute_iconos(ui_overview_view_t *ov)
{
    const alarma_tipo_t tipos[4] = { ALARMA_AGUA, ALARMA_GRISES,
                                     ALARMA_BATERIA, ALARMA_CONGELADOR };
    lv_obj_t *iconos[4] = { NULL, NULL, NULL, NULL };
    if (!ov) return;
    iconos[0] = ov->ic_mute_s1;
    iconos[1] = ov->ic_mute_r1;
    iconos[2] = ov->ic_mute_bat;
    iconos[3] = ov->ic_mute_frigo;
    for (int i = 0; i < 4; i++) {
        if (!iconos[i]) continue;
        bool activa = alarma_activa(tipos[i]);
        if (!activa) {
            lv_obj_add_flag(iconos[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        bool callada = alarma_silenciada(tipos[i]);
        /* Con el sonido habilitado, el altavoz CON ondas (VOLUME_MAX) invita a
         * tocarlo para callarlo; callada, el altavoz TACHADO y en gris, que se
         * lee de un vistazo sin tener que aprender nada. */
        lv_label_set_text(iconos[i], callada ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(iconos[i],
                                    callada ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
        lv_obj_clear_flag(iconos[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Callbacks de toque en las tarjetas para silenciar la alarma correspondiente.
 * El pitido y las temporizaciones los lleva alarma_estado.c; aqui solo se pide.
 *
 * En esta pantalla el toque en la tarjeta ADEMAS abre su pantalla de detalle
 * (el historico de bateria, la grafica de temperaturas): es el atajo que ya
 * existia y el usuario decidio el 14-sep-2026 que se quede como esta. En la
 * cabina 3,5", que no tiene esas pantallas, la tarjeta solo silencia. */
static void alarm_mute_s1_cb(lv_event_t *e)
{
    (void)e;
    alarma_silenciar(ALARMA_AGUA);
}
static void alarm_mute_r1_cb(lv_event_t *e)
{
    (void)e;
    alarma_silenciar(ALARMA_GRISES);
}
static void alarm_mute_soc_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    alarma_silenciar(ALARMA_BATERIA);
    if (ov) ui_show_battery_history_screen(ov->base.ui);
}

static void alarm_mute_freezer_cb(lv_event_t *e)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)lv_event_get_user_data(e);
    alarma_silenciar(ALARMA_CONGELADOR);
    if (ov) ui_show_chart_screen(ov->base.ui);
}

/* Aviso flotante de las alarmas: se movio a alarma_estado.c (ver alli). Aqui
 * solo queda el gesto que silencia: tocar el tanque (o la card de bateria, o
 * la temperatura del congelador) YA la silenciaba, y el aviso global dice
 * exactamente eso. */

static void overview_camper_tick_cb(lv_timer_t *t)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov || !ov->base.root) return;
    if (lv_obj_has_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN)) return;
    /* En modo Rotar el overview queda tapado por overlays / se rota a otra
     * vista: no renderizar (evita redraws invisibles y consumo de heap LVGL).
     * En modo Atenuar la pantalla solo baja de brillo pero SIGUE visible, asi
     * que hay que seguir renderizando para que los datos no se congelen. */
    if (ov->base.ui && ov->base.ui->screensaver.active &&
        ov->base.ui->screensaver.mode == UI_SCREENSAVER_MODE_ROTATE) return;
    overview_render(ov);
}

/* Timer aparte a 150 ms solo para la animacion de rotacion del icono
 * del ventilador. La velocidad de giro se escala con fan_percent:
 *   100 % → 180 deg/s = una vuelta cada 2 segundos.
 *     0 % → no se mueve. */
static void overview_fan_rotate_cb(lv_timer_t *t)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)t->user_data;
    if (!ov || !ov->img_fan) return;
    if (lv_obj_has_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN)) return;
    /* Solo detenemos la animacion del ventilador en modo Rotar (vista tapada,
     * y evita agotar el heap LVGL con buffers de rotacion). En modo Atenuar la
     * vista sigue visible, asi que el ventilador debe seguir girando. */
    if (ov->base.ui && ov->base.ui->screensaver.active &&
        ov->base.ui->screensaver.mode == UI_SCREENSAVER_MODE_ROTATE) return;
    frigo_state_t fs_copy;
    frigo_get_state_copy(&fs_copy);
    const frigo_state_t *fs = &fs_copy;
    if (!fs) return;
    uint8_t p = fs->fan_percent;
    if (p == 0) return;
    /* delta_deci en decimas de grado por tick (150 ms). A 100 % giramos
     * 270 deci/tick = 27 deg/tick = 180 deg/s = una vuelta cada 2 segundos.
     * A 50 % seria 13.5 deg/tick = 90 deg/s = 4 segundos por vuelta. */
    /* delta_deci en decimas de grado por tick (150 ms).
     * A 100 % -> 270 deci/tick = 27 deg/tick = 180 deg/s (mismo que antes). */
    int delta = (p * 270) / 100;
    ov->fan_angle_deci += delta;
    if (ov->fan_angle_deci >= 3600) ov->fan_angle_deci -= 3600;
    lv_img_set_angle(ov->img_fan, ov->fan_angle_deci);
}

/* Pinta un LED redondo de estado: verde con halo cuando esta encendido y gris
 * apagado cuando no. Lo usan los tres botones camper y el LED del 230 V, para
 * que los cuatro hablen el mismo idioma (5-oct-2026). */
static void camper_led_paint(lv_obj_t *led, bool on)
{
    if (!led) return;
    /* Si el dedo está encima, manda el toque (ver camper_led_touch). */
    if (lv_obj_get_user_data(led)) return;
    lv_obj_set_style_bg_color(led, on ? UI_COLOR_GREEN : UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_shadow_width(led, on ? 10 : 0, 0);
    lv_obj_set_style_shadow_color(led, UI_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_opa(led, on ? LV_OPA_80 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_spread(led, on ? 2 : 0, 0);
}

/* Acuse del TOQUE en el LED: mientras el dedo está sobre el botón, el LED se
 * enciende en verde; al soltar vuelve a pintarse con el ESTADO de verdad (lo
 * hace el refresco). Pedido del dueño el 6-nov-2026: "el LED no está verde"
 * al pulsar. */
static void camper_led_touch(lv_obj_t *led, bool tocando)
{
    if (!led) return;
    lv_obj_set_user_data(led, (void *)(intptr_t)(tocando ? 1 : 0));
    if (!tocando) return;   /* al soltar lo repinta el refresco con el estado */
    lv_obj_set_style_bg_color(led, UI_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_width(led, 10, 0);
    lv_obj_set_style_shadow_color(led, UI_COLOR_GREEN, 0);
    lv_obj_set_style_shadow_opa(led, LV_OPA_80, 0);
    lv_obj_set_style_shadow_spread(led, 2, 0);
}

static void camper_btn_event_cb(lv_event_t *e)
{
    char cmd = (char)(intptr_t)lv_event_get_user_data(e);
    /* Auto-marker en log para correlacionar pulsacion con tramas RX */
    const char *name = (cmd == 'i') ? "BTN Luz INT" :
                       (cmd == 'o') ? "BTN Luz EXT" :
                       (cmd == 'p') ? "BTN Bomba" : "BTN ?";
    ne185_log_marker(name);
    ne185_send_cmd(cmd);
}

/* (helper camper_make_tank antiguo eliminado: ahora se usa
 *  ui_tank_create de ui_card.c, que es el widget visual grande) */

/* Fuente de UN SOLO GLIFO para el grifo de la Bomba, generada con
 * lv_font_conv a partir de Material Design Icons (mdi-faucet, U+F1B29), como se
 * hizo con el rayo y el panel solar. En FontAwesome libre NO hay grifo, y
 * dibujarlo con formas salia a cartel de barritas (lo dijo el usuario): con el
 * glifo se ve como la bombilla de las luces. */
/* 64 px: el grifo es el UNICO contenido del ovalo de la Bomba y con 40 se
 * quedaba pequeño (lo dijo el usuario: "el icono ok pero mas grande"). */
LV_FONT_DECLARE(mdi_faucet_64);

/* Dibujo del icono de un boton camper: la BOMBILLA con rayos de las luces y el
 * GRIFO de la Bomba. Los dos van DIBUJADOS con formas (FontAwesome libre no trae
 * ni rayos ni un grifo claro), como el deposito de aguas grises de ui_card.c:
 * piezas sobre la rejilla de 4 px, colocadas a mano con IGNORE_LAYOUT y sin
 * robarle el toque al boton. */
typedef enum {
    CAMPER_ICON_BULB = 0,    /* bombilla con rayos (Luz INT / Luz EXT) */
    CAMPER_ICON_TAP,         /* grifo (Bomba) */
} camper_icon_t;

/* Bombilla CON RAYOS, como el pictograma del panel fisico: el glifo FontAwesome
 * de la bombilla en el centro y seis trazos cortos alrededor. */
static lv_obj_t *camper_make_bulb_icon(lv_obj_t *parent, lv_color_t color)
{
    static const struct { lv_coord_t x, y, dx, dy; } rayos[] = {
        {  4, 24,  6,  0 },   /* izquierda */
        { 44, 24, -6,  0 },   /* derecha */
        {  9, 11,  5, -5 },   /* arriba-izquierda */
        { 39, 11, -5, -5 },   /* arriba-derecha */
        {  9, 37,  5,  5 },   /* abajo-izquierda */
        { 39, 37, -5,  5 },   /* abajo-derecha */
    };
    /* OJO: lv_line NO copia los puntos, se queda con el puntero -> tienen que
     * vivir en un array estatico (uno por rayo) que se rellena una sola vez. */
    static lv_point_t pts[sizeof(rayos) / sizeof(rayos[0])][2];
    static bool pts_listos = false;
    if (!pts_listos) {
        for (size_t i = 0; i < sizeof(rayos) / sizeof(rayos[0]); i++) {
            pts[i][0].x = 0;           pts[i][0].y = 0;
            pts[i][1].x = rayos[i].dx; pts[i][1].y = rayos[i].dy;
        }
        pts_listos = true;
    }

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 48, 48);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bombilla = lv_label_create(box);
    lv_obj_set_style_text_font(bombilla, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(bombilla, color, 0);
    lv_label_set_text(bombilla, "\xEF\x83\xAB");   /* U+F0EB = fa-lightbulb */
    lv_obj_center(bombilla);

    for (size_t i = 0; i < sizeof(rayos) / sizeof(rayos[0]); i++) {
        lv_obj_t *r = lv_line_create(box);
        lv_line_set_points(r, pts[i], 2);
        lv_obj_set_style_line_width(r, 2, 0);
        lv_obj_set_style_line_color(r, color, 0);
        lv_obj_set_style_line_rounded(r, true, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(r, LV_ALIGN_TOP_LEFT, rayos[i].x, rayos[i].y);
    }
    return box;
}

/* Crea un botón ÓVALO como los del panel fisico del usuario: contorno fino del
 * color de la funcion, SIN relleno (se ve el fondo de la tarjeta) y sin sombra,
 * con el dibujo dentro, su texto corto si lo lleva y el LED redondo de estado
 * asomando por el borde. El LED queda accesible via lv_obj_get_user_data(btn).
 * `text` puede ser NULL (la Bomba va sin rotulo). El LED de estado va DENTRO
 * del oval, a la IZQUIERDA, en los tres botones (6-oct-2026). */
/* ── BOTÓN DEL PANEL CAMPER (06-nov-2026) ──────────────────────────────────
 *
 * El óvalo se dibuja con DOS objetos y no con `border_width`:
 *   - el botón, relleno ENTERO del color de la función (es el FILO: 2 px);
 *   - dentro, un "cuerpo" 4 px más pequeño con el color oscuro de pulsador.
 * Con border_width el filo se perdía por arriba y por abajo (en las curvas y
 * en el tramo recto, según el estado) y el óvalo quedaba abierto; con dos
 * óvalos concéntricos el aro sale igual de grueso en toda la vuelta.
 *
 * Dentro del cuerpo, las tres piezas van colocadas a mano para que los tres
 * botones repartan igual: LED a la izquierda, dibujo en el centro exacto y
 * rótulo a la derecha (la Bomba no lleva).
 *
 * Al pulsar, el CUERPO se aclara (como un pulsador de verdad); el filo y el
 * dibujo no cambian, y el LED no se toca: el LED es el estado de la luz o de
 * la bomba, no el toque. */
#define CAMPER_BTN_W      200
#define CAMPER_BTN_H       85
#define CAMPER_BTN_FILO     2

static void camper_btn_press_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_user_data(e);
    if (!btn) return;
    lv_obj_t *cuerpo = lv_obj_get_child(btn, 0);
    lv_obj_t *led    = (lv_obj_t *)lv_obj_get_user_data(btn);
    lv_event_code_t code = lv_event_get_code(e);
    bool pulsado = (code == LV_EVENT_PRESSED);
    if (cuerpo) {
        lv_obj_set_style_bg_color(cuerpo,
            pulsado ? UI_COLOR_CARD_HI : UI_COLOR_CARD_LO, 0);
    }
    camper_led_touch(led, pulsado);
}

static lv_obj_t *camper_make_button(lv_obj_t *parent, const char *text,
                                    char cmd_char, lv_color_t accent,
                                    camper_icon_t dibujo)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, CAMPER_BTN_W, CAMPER_BTN_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    /* El botón ES el filo: relleno entero del color de la función. */
    lv_obj_set_style_bg_color(btn, accent, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* Cuerpo: 2 px por dentro en todo el perímetro. */
    lv_obj_t *cuerpo = lv_obj_create(btn);
    lv_obj_remove_style_all(cuerpo);
    lv_obj_set_size(cuerpo, CAMPER_BTN_W - 2 * CAMPER_BTN_FILO,
                            CAMPER_BTN_H - 2 * CAMPER_BTN_FILO);
    lv_obj_center(cuerpo);
    lv_obj_set_style_radius(cuerpo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cuerpo, UI_COLOR_CARD_LO, 0);
    lv_obj_set_style_bg_opa(cuerpo, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cuerpo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cuerpo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, camper_btn_press_cb, LV_EVENT_PRESSED,     btn);
    lv_obj_add_event_cb(btn, camper_btn_press_cb, LV_EVENT_RELEASED,    btn);
    lv_obj_add_event_cb(btn, camper_btn_press_cb, LV_EVENT_PRESS_LOST,  btn);
    lv_obj_add_event_cb(btn, camper_btn_press_cb, LV_EVENT_CLICKED,     btn);

    /* LED de estado: a la izquierda del cuerpo. */
    lv_obj_t *led = lv_obj_create(cuerpo);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, 20, 20);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE);
    /* LED 20 px más a la derecha (06-nov-2026, pedido del dueño): a 6 px del
     * filo quedaba pegado a la curva. */
    lv_obj_align(led, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_set_user_data(btn, led);

    /* Dibujo de la función, en el centro exacto. */
    if (dibujo == CAMPER_ICON_TAP) {
        lv_obj_t *l_tap = lv_label_create(cuerpo);
        lv_obj_set_style_text_font(l_tap, &mdi_faucet_64, 0);
        lv_obj_set_style_text_color(l_tap, accent, 0);
        lv_label_set_text(l_tap, "\xF3\xB1\xAC\xA9");   /* U+F1B29 = mdi-faucet */
        lv_obj_align(l_tap, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_t *bulb = camper_make_bulb_icon(cuerpo, accent);
        lv_obj_align(bulb, LV_ALIGN_CENTER, 0, 0);
    }

    /* Rótulo corto ("INT"/"EXT") pegado a la derecha. */
    if (text && text[0]) {
        lv_obj_t *l_text = lv_label_create(cuerpo);
        /* 24 en vez de 28 (06-nov-2026, pedido del dueño): el rótulo pesaba
         * más que el dibujo. Va alineado a la derecha, así que al encoger solo
         * se mueve su propio borde izquierdo: nada más cambia de sitio. */
        lv_obj_set_style_text_font(l_text, &lv_font_montserrat_24_es, 0);
        lv_obj_set_style_text_color(l_text, UI_COLOR_TEXT_SOFT, 0);
        lv_label_set_text(l_text, text);
        lv_obj_align(l_text, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    lv_obj_add_event_cb(btn, camper_btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)cmd_char);
    return btn;
}

ui_device_view_t *ui_overview_view_create(ui_state_t *ui, lv_obj_t *parent)
{
    /* Widgets nuevos: el cache de lo ultimo pintado no vale (O1). */
    s_ov_last_s1 = 0xFE;
    s_ov_last_r1 = 0xFE;
    if (!ui || !parent) return NULL;
    ui_overview_view_t *ov = calloc(1, sizeof(*ov));
    if (!ov) return NULL;

    ov->base.ui = ui;
    ov->base.root = lv_obj_create(parent);
    lv_obj_set_size(ov->base.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(ov->base.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ov->base.root, 0, 0);
    lv_obj_set_style_pad_all(ov->base.root, 8, 0);
    /* A TODO EL ANCHO Y ARRIBA DEL TODO (05-nov-2026). El margen real que sobraba
     * venia del contenedor de pestanas (ver ui.c), no de aqui; ahora los
     * laterales se quedan en 8 px y arriba/abajo en 4, que es lo justo para que
     * las tarjetas no toquen el borde fisico. */
    lv_obj_set_style_pad_hor(ov->base.root, 8, 0);
    lv_obj_set_style_pad_top(ov->base.root, 4, 0);
    lv_obj_set_style_pad_bottom(ov->base.root, 4, 0);
    lv_obj_set_style_pad_gap(ov->base.root, 8, 0);
    lv_obj_set_layout(ov->base.root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ov->base.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov->base.root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ov->base.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov->base.root, LV_OBJ_FLAG_HIDDEN);

    /* ── Grid principal: columna con [fila de 3 cards] + [fila indicadores] ── */
    lv_obj_t *grid = lv_obj_create(ov->base.root);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_flex_grow(grid, 3);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* Fila superior: las 3 cards Victron (Solar, Bateria, DC/DC), sin tocar.
     * cards_row=4 + ind_strip=1 -> las cards ocupan el 80% del alto (igual
     * que antes, cuando cada columna era card 80% + indicador 20%). */
    lv_obj_t *cards_row = lv_obj_create(grid);
    lv_obj_remove_style_all(cards_row);
    lv_obj_set_width(cards_row, lv_pct(100));
    /* EL ALTO QUE TENIAN ANTES, FIJO (05-nov-2026, pedido del dueño: las tres de
     * arriba con su tamaño de siempre y lo que se gane, para las dos de abajo).
     * Antes de quitar el relleno de las pestanas la fila medía 298 px; con
     * flex_grow(4) se repartia el alto ganado y crecia ella tambien. Ahora no
     * crece: el hueco extra se lo queda la fila de abajo (camper y frigo).
     * REDONDEADO A LA REJILLA (9-nov-2026): 298 no es multiplo de 4 y el CI lo
     * rechaza; 296 deja los mismos 544 px con la fila de abajo (296+8+240), que
     * es el alto que hay de verdad. Medido con /captura: las tarjetas de arriba
     * se encogen 2 px (son pct(100) de la columna y reparten igual) y las dos de
     * abajo ganan esos 2; nada se solapa ni se recorta. */
    lv_obj_set_height(cards_row, 296);
    lv_obj_set_layout(cards_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cards_row, 8, 0);
    lv_obj_clear_flag(cards_row, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Columna izquierda: card Solar ── */
    lv_obj_t *col_left = lv_obj_create(cards_row);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_height(col_left, lv_pct(100));
    lv_obj_set_flex_grow(col_left, 3);
    lv_obj_set_layout(col_left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_left, 8, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);
    /* 4 px de menos por abajo (09-nov-2026). La columna mide un pelin mas de
     * lo que se ve en la pestana y ademas la tarjeta se salia con su contenido:
     * con 1 px el filo inferior caia justo en el limite del recorte y la
     * tarjeta se veia SIN linea abajo (lo reporto el dueño). Con 4 (valor de la
     * rejilla, sin excepciones) el filo queda 4 px dentro de la zona visible y
     * se ve entero; el contenido de la tarjeta se sube esos mismos 4 px (ver
     * pad_top de cada card). */
    lv_obj_set_style_pad_bottom(col_left, 4, 0);

    ov->card_solar = create_node_card(col_left, &icon_solar,
                                      "Solar", UI_COLOR_GREEN, &ov->m_solar_w);
    lv_obj_set_width(ov->card_solar, lv_pct(100));
    /* ALTO = 100 % DE LA COLUMNA, no SIZE_CONTENT (09-nov-2026). Con
     * SIZE_CONTENT la card medía lo que su contenido, que es 2-3 px MÁS que la
     * columna; la columna le recortaba esos píxeles y con ellos el borde de
     * abajo: la card se veía sin linea inferior (lo reportó el dueño: "solar,
     * bateria y dc/dc solo"). Con pct(100) la card acaba justo donde acaba la
     * columna y el borde entra entero. */
    lv_obj_set_height(ov->card_solar, lv_pct(100));
    /* Subir el contenido 5 px sin alterar el tamano externo de la card:
     * reducimos el pad superior y aumentamos el inferior en la misma
     * cantidad. La altura es la misma porque la fija el flex_grow. */
    /* El contenido 4 px MAS ARRIBA (09-nov-2026, peticion del dueño: "que
     * suba esos pixeles"). Desde que la card mide el 100 % de la columna, el
     * contenido quedaba 2-3 px justo y se cortaba por abajo; subiendolo 4 el
     * borde inferior entra entero y no se recorta nada. La caja del contenido
     * sigue midiendo lo mismo: se quita arriba y se pone abajo. */
    lv_obj_set_style_pad_top(ov->card_solar, UI_PAD_CARD - 9, 0);
    lv_obj_set_style_pad_bottom(ov->card_solar, UI_PAD_CARD + 9, 0);
    /* Reducir el gap entre header/Actual/Hoy de 16 a 8 px: la 2ª métrica
     * se salía por abajo en columnas estrechas. */
    lv_obj_set_style_pad_gap(ov->card_solar, 8, 0);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_solar_w, "Actual", UI_COLOR_TEXT_SOFT);
    ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT);
    ov->m_solar_kwh = ui_metric_create_compact(ov->card_solar, "Hoy");
    ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);
    /* La tarjeta Solar estrena el acabado (ver ov_estilo_tarjeta). */
    ov_estilo_tarjeta(ov->card_solar, UI_COLOR_GREEN, "SOLAR", ov->m_solar_w, ov->m_solar_kwh);

    ov->pill_solar_state = ov_make_state_pill(ov->card_solar);
    lv_obj_add_event_cb(ov->pill_solar_state, ov_solar_state_pill_cb,
                        LV_EVENT_CLICKED, ov);
    /* Pulsar la card (fuera del pill) abre el detalle Solar a pantalla completa. */
    lv_obj_add_flag(ov->card_solar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->card_solar, ov_card_solar_clicked, LV_EVENT_CLICKED, ov);

    /* (Agua limpia movida a la fila de indicadores de abajo) */

    /* ── Columna central: card Bateria ── */
    lv_obj_t *col_center = lv_obj_create(cards_row);
    lv_obj_remove_style_all(col_center);
    lv_obj_set_height(col_center, lv_pct(100));
    lv_obj_set_flex_grow(col_center, 5);
    lv_obj_set_layout(col_center, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_center, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_center, 8, 0);
    lv_obj_clear_flag(col_center, LV_OBJ_FLAG_SCROLLABLE);
    /* 4 px de menos por abajo, igual que col_left (ver el comentario de alli). */
    lv_obj_set_style_pad_bottom(col_center, 4, 0);

    ov->card_bat = ui_card_create(col_center, UI_COLOR_ORANGE);
    lv_obj_set_width(ov->card_bat, lv_pct(100));
    /* 8:3 igual que los laterales (col_left card_solar=8, tank_s1=3 y
     * col_right card_loads=8, tank_r1=3) para que card_bat tenga la misma
     * altura que Solar/DC/DC y el bottom_row gane espacio suficiente para
     * que la card frigo se vea entera. */
    lv_obj_set_height(ov->card_bat, lv_pct(100));   /* ver card_solar */
    /* Subir el contenido 10 px en total (los 5 px del cambio anterior +
     * 5 px adicionales pedidos al "subir la card 5"). Reducimos pad_top
     * a UI_PAD_CARD-10 y aumentamos pad_bottom a UI_PAD_CARD+10 para no
     * tocar el tamano externo de la card. */
    lv_obj_set_style_pad_top(ov->card_bat, UI_PAD_CARD - 14, 0);   /* 4 px mas arriba, ver card_solar */
    /* Relleno de abajo 26 -> 14 (06-nov-2026): el aro grande (200 px) no
     * cabía en el alto que quedaba y se cortaba por arriba; con 12 px más de
     * fila entra entero. El contenido baja con él, que es justo lo que se
     * buscaba. */
    lv_obj_set_style_pad_bottom(ov->card_bat, UI_PAD_CARD + 2, 0);
    ui_card_set_title_img(ov->card_bat, &icon_battery,
                          "Batería", UI_COLOR_ORANGE);
    /* Click en la card de bateria silencia la alarma de SOC */
    lv_obj_add_flag(ov->card_bat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->card_bat, alarm_mute_soc_cb, LV_EVENT_CLICKED, ov);
    /* Mismo acabado que Solar y DC/DC. */
    ov_estilo_marco(ov->card_bat, UI_COLOR_ORANGE);
    ov_estilo_cabecera(ov->card_bat, "BATERÍA");
    /* Cuerpo de card_bat: fila horizontal con [Corriente · Arc SoC · Autonomía].
     * Aprovecha el ancho disponible en lugar del alto para que con la card
     * a 8:3 (igual altura que Solar/DC/DC) las métricas no se recorten. */
    lv_obj_t *bat_row = lv_obj_create(ov->card_bat);
    lv_obj_remove_style_all(bat_row);
    lv_obj_set_width(bat_row, lv_pct(100));
    lv_obj_set_flex_grow(bat_row, 1);
    lv_obj_set_layout(bat_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bat_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_row, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bat_row, LV_OBJ_FLAG_SCROLLABLE);
    ov->m_bat_current = ui_metric_create_compact(bat_row, "Corriente");
    /* El dibujo de batería de coche se cambia por un ARO de estado de carga
     * (6-nov-2026): el dueño dijo que el dibujo no le convencía. En el aro el
     * estado se lee de dos maneras a la vez, la visual (cuánto arco está
     * pintado, y de qué color) y la numérica (67.4 % grande en el centro, con
     * la tensión debajo). Ocupa el mismo hueco que el dibujo: 150x150. */
    ov->arc_soc       = ui_battery_ring_create(bat_row, 200);
    ov->m_ttg         = ui_metric_create_compact(bat_row, "Autonomía");
    /* Las dos metricas laterales con ancho base 0 + flex_grow=1: se reparten
     * por igual el espacio a los lados del arc SoC (150 px fijo), asi su ancho
     * NO depende del texto. Antes eran SIZE_CONTENT y, al cambiar el valor de
     * corriente, SPACE_AROUND recolocaba y el icono de bateria se desplazaba. */
    lv_obj_set_width(ov->m_bat_current, 0);
    lv_obj_set_flex_grow(ov->m_bat_current, 1);
    lv_obj_set_width(ov->m_ttg, 0);
    lv_obj_set_flex_grow(ov->m_ttg, 1);
    /* Bajar fuentes para que las metricas no invadan el arc SoC en el ancho
     * disponible (col_center ~320 px, arc 150 px fijo ⇒ menos sitio por metrica).
     * title 24→20, value 46→24, unit 24→20. Desplazar 10 px hacia abajo. */
    {
        lv_obj_t *metrics[] = { ov->m_bat_current, ov->m_ttg };
        for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
            lv_obj_t *title = lv_obj_get_child(metrics[i], 0);
            lv_obj_t *row   = lv_obj_get_child(metrics[i], 1);
            if (title) {
                lv_obj_set_style_text_font(title, UI_FONT_TEXT, 0);
                lv_obj_set_style_text_color(title, UI_COLOR_TEXT_DIM, 0);
            }
            if (row) {
                lv_obj_t *value = lv_obj_get_child(row, 0);
                lv_obj_t *unit  = lv_obj_get_child(row, 1);
                /* 24, LA MISMA que el valor de Autonomia (que bajo a 24
                 * porque "12h 30m" no cabia). Con 32 y 24 juntas, las dos
                 * cifras y sus rotulos quedaban a distinta altura y la fila
                 * parecia torcida (visto en la captura del 22-sep-2026). */
                /* 24 y no 28: es el tamaño en el que ENTRAN los valores
                 * reales más largos sin recortarse ni mover nada:
                 *   Corriente  "-199.9 A" = 103 px   (hueco: 108)
                 *   Autonomía  "23h 59m"  =  75 px
                 * A 28 el de corriente medía 119 y se metía debajo del aro. */
                if (value) lv_obj_set_style_text_font(value, &lv_font_montserrat_24_es, 0);
                if (unit)  lv_obj_set_style_text_font(unit,  &lv_font_montserrat_20_es, 0);
            }
            /* Bajadas 12 px más (44 -> 56) para dejar sitio al aro grande:
             * el disco manda en el centro y las dos cifras se alinean a su
             * altura baja, como en un cuadro de mandos. */
            lv_obj_set_style_translate_y(metrics[i], 68, 0);
        }
    }
    /* La caja de bateria mas ancha deja menos sitio a los lados: bajar la
     * fuente del valor de Autonomia (texto largo tipo "12h 30m") para que no
     * se corte a la derecha. */
    {
        lv_obj_t *ttg_row = lv_obj_get_child(ov->m_ttg, 1);
        lv_obj_t *ttg_val = ttg_row ? lv_obj_get_child(ttg_row, 0) : NULL;
        if (ttg_val)
            lv_obj_set_style_text_font(ttg_val, &lv_font_montserrat_24_es, 0);
    }

    /* (Aguas grises movida a la fila de indicadores de abajo) */

    /* ── Columna derecha: card DC/DC ── */
    lv_obj_t *col_right = lv_obj_create(cards_row);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_height(col_right, lv_pct(100));
    lv_obj_set_flex_grow(col_right, 3);
    lv_obj_set_layout(col_right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_right, 8, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);
    /* 4 px de menos por abajo, igual que las otras dos columnas (ver col_left). */
    lv_obj_set_style_pad_bottom(col_right, 4, 0);

    ov->card_loads = create_node_card(col_right, &icon_dcdc,
                                      "DC/DC", UI_COLOR_CYAN, &ov->m_loads_w);
    lv_obj_set_width(ov->card_loads, lv_pct(100));
    lv_obj_set_height(ov->card_loads, lv_pct(100));   /* ver card_solar */
    lv_obj_set_style_pad_top(ov->card_loads, UI_PAD_CARD - 9, 0);   /* 4 px mas arriba, ver card_solar */
    lv_obj_set_style_pad_bottom(ov->card_loads, UI_PAD_CARD + 9, 0);
    /* Reducir el gap entre header/Motor/Servicio de 16 a 8 px: la 2ª
     * métrica se salía por abajo en columnas estrechas. */
    lv_obj_set_style_pad_gap(ov->card_loads, 8, 0);
    /* Sin SPACE_AROUND: stack tight desde arriba, evita solape texto */
    ui_metric_set_label(ov->m_loads_w, "Motor", UI_COLOR_TEXT_SOFT);
    ui_metric_set(ov->m_loads_w, "--", "V", UI_COLOR_TEXT);
    ov->m_loads_kwh = ui_metric_create_compact(ov->card_loads, "Servicio");
    ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);
    /* El mismo acabado que la tarjeta Solar: la cifra pasa a 46 (la pone
     * ov_estilo_tarjeta) y no a 28, que era lo que había antes para dejar sitio
     * a la pastilla. Pastilla pulsable al final de la card. */
    ov_estilo_tarjeta(ov->card_loads, UI_COLOR_CYAN, "DC/DC", ov->m_loads_w, ov->m_loads_kwh);
    ov->pill_dcdc_state = ov_make_state_pill(ov->card_loads);
    lv_obj_add_event_cb(ov->pill_dcdc_state, ov_dcdc_state_pill_cb,
                        LV_EVENT_CLICKED, ov);
    /* Pulsar la card (fuera del pill) abre el detalle DC/DC a pantalla completa. */
    lv_obj_add_flag(ov->card_loads, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->card_loads, ov_card_dcdc_clicked, LV_EVENT_CLICKED, ov);

    /* Toque robusto en las 3 cards: que un tap en cualquier parte (metricas,
     * cabecera...) abra el detalle, no solo en el fondo desnudo. Los pills de
     * estado se excluyen para conservar su propio handler. */
    ov_card_make_tappable(ov->card_solar, ov->pill_solar_state);
    ov_card_make_tappable(ov->card_bat,   NULL);
    ov_card_make_tappable(ov->card_loads, ov->pill_dcdc_state);

    /* (230V movido a la fila de indicadores de abajo) */

    /* (Indicadores y botones camper movidos a la card camper de abajo) */

    /* ── Parte inferior: 2 cards ──────────────────────────────────────
     * Izquierda: card "camper" (borde cian, ancho hasta ~Luz EXT) con
     *   [fila indicadores: Agua limpia + Aguas grises + 230V] + [fila botones
     *   Luz INT/Bomba/Luz EXT].
     * Derecha: card frigo, mas alta (rellena el alto de la fila), mismo
     *   ancho que su contenido. ─────────────────────────────────────── */
    lv_obj_t *bottom_region = lv_obj_create(ov->base.root);
    ov->camper_bottom = bottom_region;
    lv_obj_remove_style_all(bottom_region);
    lv_obj_set_width(bottom_region, lv_pct(100));
    /* ALTO CALCULADO A MANO (05-nov-2026). Con flex_grow la fila crecia pero las
     * tarjetas de dentro se centraban y quedaban MAS PEQUEÑAS que antes (lo vio
     * el dueño). La pantalla es fija -- 600 px, barra de 48, 8 de margenes --,
     * asi que los altos se ponen aqui y no hay nada que adivinar:
     *   arriba 296 (298 redondeado a la rejilla) + 8 de hueco + 240 abajo = 544. */
    lv_obj_set_height(bottom_region, 240);
    lv_obj_set_layout(bottom_region, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bottom_region, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_region, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bottom_region, 8, 0);
    lv_obj_set_style_pad_hor(bottom_region, 8, 0);
    lv_obj_clear_flag(bottom_region, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Card camper (izquierda, borde cian) ──────────────────────────
     * 3 columnas a lo ancho, cada una a alto completo de la card:
     *   1) Agua limpia: tanque VERTICAL (4 LEDs apilados, llena de abajo
     *      a arriba), ocupa todo el alto.
     *   2) Indicadores: Aguas grises arriba + pill 230V debajo.
     *   3) Botones: [Luz INT + Bomba] arriba, Luz EXT debajo. ────────── */
    lv_obj_t *camper_card = lv_obj_create(bottom_region);
    lv_obj_remove_style_all(camper_card);
    lv_obj_set_flex_grow(camper_card, 1);
    lv_obj_set_height(camper_card, lv_pct(100));
    lv_obj_set_style_border_color(camper_card, UI_COLOR_VIOLET, 0);
    /* Mismo acabado que la fila de arriba (06-nov-2026): superficie un punto
     * más clara que el fondo, filo de 1 px y sombra abierta. La cambiamos de
     * paso al helper compartido para que las cinco tarjetas sean iguales. */
    ov_estilo_marco(camper_card, UI_COLOR_VIOLET);
    lv_obj_set_style_bg_opa(camper_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(camper_card, 8, 0);
    lv_obj_set_style_pad_gap(camper_card, 12, 0);
    lv_obj_set_layout(camper_card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(camper_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(camper_card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(camper_card, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Columna 1: Agua limpia vertical (alto completo) ──
     * Ancho de la caja al contenido para que el titulo "Agua limpia" entre
     * entero. El contenedor del bargraph lleva ancho fijo propio (ver
     * UI_TANK_CLEAN_H en ui_card.c), por eso aqui SIZE_CONTENT ya no colapsa
     * la escala. */
    ov->tank_s1 = ui_tank_create(camper_card, LV_SIZE_CONTENT, 1,
                                 "Agua limpia", UI_COLOR_CYAN, UI_TANK_CLEAN_H);
    /* UN POCO MAS BAJO (05-nov-2026, pedido del dueño: "el grafico y los textos
     * 1/4..4/4, un poco mas pequeño en vertical"). Con pct(100) el deposito se
     * comia todo el alto de la tarjeta (215 px con el nuevo reparto); a 188
     * (190 redondeado a la rejilla el 9-nov-2026) deja aire arriba y abajo y la
     * escala se lee igual. */
    lv_obj_set_height(ov->tank_s1, 188);
    /* Y UN POCO MAS ARRIBA (05-nov-2026, pedido del dueño): la tarjeta centra sus
     * columnas en vertical, asi que el rotulo "Agua limpia" quedaba a media altura.
     * Se sube el bloque 10 px sin tocar su tamaño. */
    lv_obj_set_style_translate_y(ov->tank_s1, -10, 0);
    lv_obj_set_width(ov->tank_s1, LV_SIZE_CONTENT);
    /* El DIBUJO del depósito, 20 px más abajo (06-nov-2026, pedido del dueño):
     * se desplazan todos los hijos menos el rótulo, así baja el contorno, la
     * boca de llenado y el agua, y el título se queda donde estaba. */
    {
        lv_obj_t *cuerpo = lv_obj_get_child(ov->tank_s1, 1);
        if (cuerpo) lv_obj_set_style_translate_y(cuerpo, 20, 0);
    }
    /* OJO: sólo el cuerpo. El tapón y la onda se recolocan solos en cada
     * refresco tomando las coordenadas del cuerpo (ui_tank_set), así que
     * desplazarlos también los movía dos veces. */
    /* El dibujo (ya desplazado) sale 20 px por debajo de la caja del widget:
     * sin esto, el widget recortaba justo esa parte y el "1/4" se veía a
     * medias. Con OVERFLOW_VISIBLE sólo recorta la tarjeta, y ahí cabe. */
    lv_obj_add_flag(ov->tank_s1, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    /* Los rótulos de los depósitos, como los títulos de tarjeta: mayúsculas
     * grises y espaciadas (06-nov-2026). El del depósito es el hijo 0 del
     * widget (ver ui_tank_create). */
    {
        lv_obj_t *t = lv_obj_get_child(ov->tank_s1, 0);
        if (t) {
            lv_label_set_text(t, "AGUA LIMPIA");
            lv_obj_set_style_text_font(t, UI_FONT_TEXT, 0);
            lv_obj_set_style_text_color(t, UI_COLOR_TEXT, 0);   /* blanco */
            lv_obj_set_style_text_letter_space(t, UI_LS_TITULO, 0);
        }
    }
    lv_obj_add_flag(ov->tank_s1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->tank_s1, alarm_mute_s1_cb, LV_EVENT_CLICKED, ov);

    /* ── Columna 2: Aguas grises (arriba) + 230V (debajo) ── */
    lv_obj_t *ind_col = lv_obj_create(camper_card);
    lv_obj_remove_style_all(ind_col);
    lv_obj_set_height(ind_col, lv_pct(100));
    lv_obj_set_width(ind_col, LV_SIZE_CONTENT);
    lv_obj_set_layout(ind_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ind_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ind_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ind_col, 12, 0);
    lv_obj_clear_flag(ind_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 230V arriba */
    {
        lv_obj_t *pill = lv_obj_create(ind_col);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(pill, 16, 0);
        /* Chip fino (06-nov-2026): filo de 1 px y fondo oscuro cuando NO hay
         * 230 V. Antes era un bloque gris claro de 2 px que pesaba más que
         * cualquier otro elemento de la tarjeta. El verde de "hay 230 V" lo
         * sigue poniendo el refresco. */
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_color(pill, UI_COLOR_CARD_BORDER, 0);
        lv_obj_set_style_bg_color(pill, UI_COLOR_CARD_LO, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 12, 0);
        lv_obj_set_style_pad_ver(pill, 8, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        /* Contenido en FILA: el texto y la onda. La pastilla es de tamano
         * CONTENIDO, asi que un hijo alineado fuera la hace crecer y acaba
         * montandose sobre el texto (medido el 5-oct-2026, cuando el LED pisaba
         * la "V"). En fila, cada cosa tiene su sitio y su hueco. */
        lv_obj_set_layout(pill, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(pill, 4, 0);

        ov->pill_shore_lbl = lv_label_create(pill);
        lv_obj_set_style_text_font(ov->pill_shore_lbl, UI_FONT_TEXT, 0);
        lv_obj_set_style_text_color(ov->pill_shore_lbl, UI_COLOR_TEXT_SOFT, 0);
        lv_label_set_text(ov->pill_shore_lbl, "230 V");

        /* Simbolito de onda (corriente alterna) pegado al texto, como se
         * escribe "230 V ~". NO lleva LED redondo: la propia pastilla ya se pone
         * verde cuando hay 230 V, asi que el redondo sobraba (6-oct-2026). */
        /* ONDA SENOIDAL de verdad (06-nov-2026, pedido del dueño: "el símbolo
         * del 230 V no es el de alterna, es triangular"). Antes eran 5 puntos
         * en zigzag y se veía un pico; ahora 17 puntos de una senoide de dos
         * ciclos, que dibujada con línea de 2 px y extremos redondeados se lee
         * como el "~" de corriente alterna. */
        static const lv_point_t onda_ac[] = {
            {0, 5}, {1, 4}, {2, 3}, {3, 2}, {4, 1}, {5, 1}, {6, 1},
            {7, 2}, {8, 3}, {9, 4}, {10, 5}, {11, 6}, {12, 7}, {13, 8},
            {14, 9}, {15, 9}, {16, 9}, {17, 8}, {18, 7}, {19, 6}, {20, 5}
        };
        lv_obj_t *ac = lv_line_create(pill);
        lv_line_set_points(ac, onda_ac, 21);
        lv_obj_set_style_line_width(ac, 2, 0);
        lv_obj_set_style_line_color(ac, UI_COLOR_TEXT, 0);
        lv_obj_set_style_line_rounded(ac, true, 0);
        lv_obj_clear_flag(ac, LV_OBJ_FLAG_CLICKABLE);

        ov->pill_shore = pill;
    }

    /* Aguas grises debajo: 1 LED rojo, ancho al contenido; alto fijo.
     * Se queda en el flujo (asi la columna conserva su ancho y el 230V su
     * sitio). Mas abajo, en build_camper, se le aplica un translate_y para
     * bajarla hasta que su base coincida con la del nivel de aguas limpias,
     * sin afectar al resto (translate es solo visual). */
    ov->tank_r1 = ui_tank_create(ind_col, LV_SIZE_CONTENT, UI_TANK_GREY_BODY_H,
                                 "Aguas grises", UI_COLOR_CYAN, UI_TANK_GREY_H);
    /* El alto se pasa UNA sola vez (UI_TANK_GREY_BODY_H). Antes se creaba con 90
     * y luego se forzaba el widget a 88: el cuerpo sobresalia 2 px por abajo y se
     * le RECORTABA el borde inferior -- el "rectangulo de aguas grises cortado"
     * que se vio el 4-oct-2026 en la pantalla principal. No volver a forzar el
     * alto por separado. */
    lv_obj_set_width(ov->tank_r1, LV_SIZE_CONTENT);
    {
        lv_obj_t *t = lv_obj_get_child(ov->tank_r1, 0);
        if (t) {
            lv_label_set_text(t, "AGUAS GRISES");
            lv_obj_set_style_text_font(t, UI_FONT_TEXT, 0);
            lv_obj_set_style_text_color(t, UI_COLOR_TEXT, 0);   /* blanco */
            lv_obj_set_style_text_letter_space(t, UI_LS_TITULO, 0);
        }
    }
    lv_obj_add_flag(ov->tank_r1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov->tank_r1, alarm_mute_r1_cb, LV_EVENT_CLICKED, ov);

    /* (El aviso flotante de alarma ya no se crea aqui: vive en
     * alarma_estado.c, sobre lv_layer_top(), para verse desde cualquier
     * pantalla aunque esta vista no se dibuje.) */

    /* Icono del altavoz de cada tarjeta que puede estar en alarma. Se crean
     * aqui (despues de las tarjetas, para que queden por encima del dibujo) y
     * los pinta refresh_mute_iconos() en cada render. Ver arriba el porque de
     * cada detalle. */
    ov->ic_mute_s1 = alarm_mute_icono_crear(ov->tank_s1, ALARMA_AGUA);
    ov->ic_mute_r1 = alarm_mute_icono_crear(ov->tank_r1, ALARMA_GRISES);
    ov->ic_mute_bat = alarm_mute_icono_crear(ov->card_bat, ALARMA_BATERIA);
    if (ov->card_bat) lv_obj_move_foreground(ov->ic_mute_bat);
    /* El del congelador va en su hueco dentro de la card frigo, que no es un
     * objeto suelto de esta funcion: se crea alli (ver el bloque de card_fridge
     * mas abajo), donde se conoce el objeto del congelador. */

    /* ── Columna 3: botones [Luz INT + Bomba] arriba, Luz EXT debajo ── */
    lv_obj_t *btn_col = lv_obj_create(camper_card);
    lv_obj_remove_style_all(btn_col);
    lv_obj_set_height(btn_col, lv_pct(100));
    lv_obj_set_width(btn_col, LV_SIZE_CONTENT);
    lv_obj_set_layout(btn_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_col, 12, 0);
    lv_obj_clear_flag(btn_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top_group = lv_obj_create(btn_col);
    lv_obj_remove_style_all(top_group);
    lv_obj_set_size(top_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(top_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(top_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(top_group, 20, 0);
    lv_obj_clear_flag(top_group, LV_OBJ_FLAG_SCROLLABLE);
    /* LOS DOS DE ARRIBA, 8 px MAS ALTOS, sin cambiarles el tamaño (05-nov-2026,
     * pedido del dueño). Se desplaza el grupo, no las pildoras: asi los dos se
     * mueven juntos y la separacion entre ellos sigue igual. */
    lv_obj_set_style_translate_y(top_group, -8, 0);

    /* Icono bombilla FA5 (0xF0EB) en UTF-8 = "\xEF\x83\xAB" */
    /* Los tres son el pictograma del panel fisico, con el LED en su borde:
     * las luces con la bombilla y sus rayos + "INT"/"EXT", y la Bomba con el
     * grifo y sin rotulo (6-oct-2026). */
    ov->btn_lin  = camper_make_button(top_group, "INT", 'i', UI_COLOR_YELLOW,
                                       CAMPER_ICON_BULB);
    ov->btn_pump = camper_make_button(top_group, NULL, 'p', UI_COLOR_CYAN,
                                       CAMPER_ICON_TAP);
    ov->btn_lout = camper_make_button(btn_col, "EXT", 'o', UI_COLOR_YELLOW,
                                       CAMPER_ICON_BULB);

    /* ── Card frigo (derecha, mas alta: rellena el alto de la fila;
     *    ancho = su contenido, igual que antes). ── */
    {
        lv_obj_t *card_fridge = lv_obj_create(bottom_region);
        lv_obj_remove_style_all(card_fridge);
        /* ANCHO FIJO (06-nov-2026). Estaba a LV_SIZE_CONTENT y la tarjeta
         * medía lo que midiera su contenido: al cambiar la cifra de la
         * temperatura ("-9.5" es más estrecho que "-16.7") la tarjeta se
         * hacía más ancha o más estrecha y EMPUJABA a la tarjeta Camper. Con
         * 192 px fijos, la temperatura cambia dentro y las tarjetas no se
         * mueven. */
        lv_obj_set_width(card_fridge, 192);
        lv_obj_set_height(card_fridge, lv_pct(100));
        lv_obj_set_style_border_color(card_fridge, UI_COLOR_ICE, 0);
        /* Mismo marco que el resto (el interior de esta tarjeta se revisa
         * aparte): si no, al lado de la Camper cantaba la diferencia. */
        ov_estilo_marco(card_fridge, UI_COLOR_ICE);
        lv_obj_set_style_bg_opa(card_fridge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(card_fridge, 12, 0);
        /* Arriba 12 y abajo 24 (abajo no 6: el aro del ventilador de 92 px quedaba
         * pegado al borde inferior, visto el 22-sep). El grupo y el ventilador
         * van en el MISMO flex centrado, asi que estos rellenos mueven a los dos:
         * la pareja de valores esta elegida para que el grupo no se pegue al
         * borde de arriba Y el ventilador no se salga por abajo (dos intentos
         * fallidos el 6-oct-2026: con 40/16 el ventilador se salia, y con una
         * columna de alto fijo tambien). Lo aprobado por el usuario es ESTO: si
         * se toca, mirar la pantalla. */
        lv_obj_set_style_pad_top(card_fridge, 12, 0);
        lv_obj_set_style_pad_bottom(card_fridge, 24, 0);
        lv_obj_set_style_pad_gap(card_fridge, 12, 0);
        lv_obj_set_layout(card_fridge, LV_LAYOUT_FLEX);
        /* Disposicion vertical: Congelador (icono+texto+temp) arriba y el
         * ventilador debajo, ambos centrados. */
        lv_obj_set_flex_flow(card_fridge, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card_fridge, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card_fridge, LV_OBJ_FLAG_SCROLLABLE);

        /* Icono del altavoz de la alarma del congelador. Se crea DESPUES del
         * contenido (mas abajo, junto al resto de la card) para que quede por
         * encima; aqui solo se reserva el sitio en el struct. */

        /* Congelador (arriba dentro de la card). El toque (silenciar alarma +
         * abrir logs del frigo) se gestiona a nivel de card_fridge, para que
         * responda en TODA la card, no solo aqui. */
        lv_obj_t *col_freezer = lv_obj_create(card_fridge);
        lv_obj_remove_style_all(col_freezer);
        lv_obj_set_size(col_freezer, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(col_freezer, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(col_freezer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col_freezer, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(col_freezer, 4, 0);

        lv_obj_t *t_row = lv_obj_create(col_freezer);
        lv_obj_remove_style_all(t_row);
        lv_obj_set_size(t_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(t_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(t_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(t_row, 4, 0);

        /* Rótulo como los títulos de tarjeta: mayúsculas, gris y espaciado
         * (06-nov-2026). El cian se queda en el termómetro y en el filo. */
        lv_obj_t *t_lbl = lv_label_create(t_row);
        /* SIN termómetro y el rótulo a 20 (06-nov-2026, pedido del dueño): al
         * quitar el icono cabe. "CONGELADOR" a 20 mide 150 px y con 1 px de
         * espaciado 159, que entra en los 164 libres al dejar el relleno
         * lateral de la tarjeta en 12. */
        lv_obj_set_style_text_font(t_lbl, UI_FONT_TEXT, 0);
        lv_obj_set_style_text_color(t_lbl, UI_COLOR_TEXT, 0);   /* blanco */
        lv_obj_set_style_text_letter_space(t_lbl, UI_LS_AJUSTE, 0);
        lv_label_set_text(t_lbl, "CONGELADOR");
        /* Valor + unidad en una fila: el numero va grande (46, solo digitos
         * que esa fuente si tiene) y la unidad "C" en 28_es, que incluye el
         * glifo grado (la 46 no lo lleva). Alineados por abajo. */
        lv_obj_t *temp_row = lv_obj_create(col_freezer);
        lv_obj_remove_style_all(temp_row);
        lv_obj_set_size(temp_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(temp_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(temp_row, 4, 0);

        ov->lbl_freezer_temp = lv_label_create(temp_row);
        lv_obj_set_style_text_font(ov->lbl_freezer_temp, UI_FONT_DISPLAY, 0);
        lv_obj_set_style_text_color(ov->lbl_freezer_temp, UI_COLOR_TEXT, 0);
        lv_label_set_text(ov->lbl_freezer_temp, " --");

        ov->lbl_freezer_unit = lv_label_create(temp_row);
        lv_obj_set_style_text_font(ov->lbl_freezer_unit, UI_FONT_TEXT, 0);
        lv_obj_set_style_text_color(ov->lbl_freezer_unit, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_pad_bottom(ov->lbl_freezer_unit, 8, 0);   /* al ras de la cifra */
        lv_label_set_text(ov->lbl_freezer_unit, "\xc2\xb0""C");
        /* Pequeno margen inferior para que la unidad case con la base del
         * numero grande en vez de pegarse al fondo del row. */
        lv_obj_set_style_pad_bottom(ov->lbl_freezer_unit, 8, 0);

        /* TENDENCIA (06-nov-2026): una flecha al lado de la cifra que dice si
         * la temperatura del congelador está subiendo o bajando en el último
         * cuarto de hora. Azul = enfriando (bien), naranja = subiendo (ojo).
         * La flecha va DIBUJADA con líneas (un "∧" o un "∨"): los glifos de
         * flecha de FontAwesome no están en la fuente _es y el fallback no los
         * saca, así que se dibuja y siempre se ve. Sin tendencia no se
         * enseña. */
        ov->ln_freezer_trend = lv_line_create(temp_row);
        lv_obj_set_size(ov->ln_freezer_trend, 14, 9);
        lv_obj_set_style_line_width(ov->ln_freezer_trend, 2, 0);
        lv_obj_set_style_line_rounded(ov->ln_freezer_trend, true, 0);
        lv_obj_set_style_line_color(ov->ln_freezer_trend, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_pad_bottom(ov->ln_freezer_trend, 12, 0);
        lv_obj_add_flag(ov->ln_freezer_trend, LV_OBJ_FLAG_HIDDEN);

        /* Ventilador (debajo dentro de la card): aro-gauge del PWM 0..100 %
         * con el ventilador girando en el centro. El nivel (0 gris ->
         * 50 naranja -> 100 rojo) lo lleva el aro; la aspa queda neutra. */
        lv_obj_t *fan_wrap = lv_obj_create(card_fridge);
        lv_obj_remove_style_all(fan_wrap);
        /* Ventilador más grande (06-nov-2026, pedido del dueño): 92 -> 108. El
         * hueco entre título/ventilador/temperatura baja de 16 a 12 para que
         * todo siga entrando en el alto de la tarjeta. */
        lv_obj_set_size(fan_wrap, 108, 108);
        lv_obj_clear_flag(fan_wrap, LV_OBJ_FLAG_SCROLLABLE);

        /* Aro: gauge de 270 grados abierto por abajo (hueco centrado en las
         * 6 en punto), se llena en sentido horario desde abajo-izquierda. */
        ov->arc_fan = lv_arc_create(fan_wrap);
        lv_obj_set_size(ov->arc_fan, 108, 108);
        lv_obj_center(ov->arc_fan);
        lv_arc_set_bg_angles(ov->arc_fan, 135, 45);
        lv_arc_set_range(ov->arc_fan, 0, 100);
        lv_arc_set_value(ov->arc_fan, 0);
        /* Es un medidor, no un control: sin knob y sin interaccion tactil. */
        lv_obj_clear_flag(ov->arc_fan, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(ov->arc_fan, NULL, LV_PART_KNOB);
        /* Aro más ancho (6 -> 9): se lee mejor el % de un vistazo. */
        lv_obj_set_style_arc_width(ov->arc_fan, 9, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ov->arc_fan, 9, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ov->arc_fan, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ov->arc_fan, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_arc_color(ov->arc_fan, UI_COLOR_TEXT_DIM, LV_PART_INDICATOR);

        /* Ventilador centrado sobre el aro; sigue girando (aspa neutra). */
        ov->img_fan = lv_img_create(fan_wrap);
        lv_img_set_src(ov->img_fan, &icon_fan);
        lv_img_set_pivot(ov->img_fan, 40, 40);
        /* La imagen mide 80 px: se escala para llenar el hueco del aro nuevo. */
        lv_img_set_zoom(ov->img_fan, 268);
        lv_obj_center(ov->img_fan);
        lv_obj_set_style_img_recolor(ov->img_fan, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_img_recolor_opa(ov->img_fan, LV_OPA_COVER, 0);
        ov->fan_angle_deci = 0;

        /* INTERCAMBIO (06-nov-2026, pedido del dueño): el VENTILADOR arriba y
         * la TEMPERATURA abajo, con el mismo tamaño y el mismo estilo. La
         * temperatura cuelga de la tarjeta (no de la columna del título) para
         * poder ponerla detrás del ventilador en el orden del flex. */
        lv_obj_set_parent(temp_row, card_fridge);
        lv_obj_move_to_index(temp_row, lv_obj_get_child_cnt(card_fridge) - 1);

        /* Toda la card del frigo es un atajo a los logs (grafica Temperaturas):
         * un toque en cualquier parte -congelador, ventilador o hueco- silencia
         * la alarma del congelador y abre la pantalla. Como los contenedores
         * hijos capturan el toque, se hace que burbujeen el evento hasta la card
         * (mismo patron que Solar/Bateria/DC-DC). */
        lv_obj_add_flag(card_fridge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card_fridge, alarm_mute_freezer_cb, LV_EVENT_CLICKED, ov);
        ov_card_make_tappable(card_fridge, NULL);

        /* Icono del altavoz, ahora que la card y su contenido existen (el
         * ultimo hijo es el que queda por encima). */
        ov->ic_mute_frigo = alarm_mute_icono_crear(card_fridge, ALARMA_CONGELADOR);
        lv_obj_move_foreground(ov->ic_mute_frigo);
    }

    /* Timer LVGL para refrescar los widgets camper aunque no llegue
     * dato Victron. Cada 500 ms re-renderiza la vista. */
    ov->camper_tick_timer = lv_timer_create(overview_camper_tick_cb, 500, ov);
    ov->fan_rotate_timer  = lv_timer_create(overview_fan_rotate_cb,  150, ov);

    /* Alarmas: la evaluacion y el pitido los lleva alarma_estado.c, con su
     * propio temporizador, para que no dependan de que ESTA vista exista o se
     * dibuje (una alarma suena tambien en Ajustes, en modo Rotar o con la vista
     * destruida). Aqui solo se arranca, y es idempotente. */
    alarma_estado_init();

    /* Defaults */
    ov->bat.ttg_min = 0xFFFFFFFF;

    ov->base.update  = overview_update;
    ov->base.show    = overview_show;
    ov->base.hide    = overview_hide;
    ov->base.destroy = overview_destroy;

    overview_render(ov);

    /* Tanques: ahora decodificados desde NE185 (clean = b[5] nibble bajo,
     * grey = b[6] nibble bajo). El refresco de los niveles llega via
     * overview_update -> ne185_data_t.s1/r1, sin necesidad de timer demo. */

    return &ov->base;
}

static void overview_update(ui_device_view_t *view, const victron_data_t *data)
{
    ui_overview_view_t *ov = (ui_overview_view_t *)view;
    if (!ov) return;
    uint32_t now = now_ms();

    if (data) {
        switch (data->type) {
            case VICTRON_BLE_RECORD_BATTERY_MONITOR: {
                const victron_record_battery_monitor_t *b = &data->record.battery;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = b->battery_current_milli;
                ov->bat.ttg_min = b->time_to_go_minutes;
                ov->bat.last_update_ms = now;
                energy_today_on_battery(b->battery_current_milli,
                                        b->battery_voltage_centi);
                break;
            }
            case VICTRON_BLE_RECORD_DCDC_CONVERTER: {
                const victron_record_dcdc_converter_t *c = &data->record.dcdc;
                ov->dcdc.has_data = true;
                ov->dcdc.state      = c->device_state;
                ov->dcdc.vin_centi  = (int16_t)c->input_voltage_centi;
                ov->dcdc.vout_centi = (int16_t)c->output_voltage_centi;
                ov->dcdc.last_update_ms = now;
                break;
            }
            case VICTRON_BLE_RECORD_ORION_XS: {
                /* Orion XS (0x0F) tiene misma semantica que DCDC_CONVERTER
                 * (input_voltage_centi = motor, output_voltage_centi = servicio,
                 *  device_state = 0 off / >0 active). El user real lleva un
                 * Orion XS, no un Orion-Tr Smart. Sin este case la card DC/DC
                 * de la vista overview se quedaba en "--" eternamente. */
                const victron_record_orion_xs_t *c = &data->record.orion;
                ov->dcdc.has_data = true;
                ov->dcdc.state      = c->device_state;
                ov->dcdc.vin_centi  = (int16_t)c->input_voltage_centi;
                ov->dcdc.vout_centi = (int16_t)c->output_voltage_centi;
                ov->dcdc.last_update_ms = now;
                break;
            }
            case VICTRON_BLE_RECORD_LYNX_SMART_BMS: {
                const victron_record_lynx_smart_bms_t *b = &data->record.lynx;
                ov->bat.has_data = true;
                ov->bat.soc_deci = b->soc_deci_percent;
                ov->bat.voltage_centi = b->battery_voltage_centi;
                ov->bat.current_milli = (int32_t)b->battery_current_deci * 100;
                ov->bat.ttg_min = b->time_to_go_min;
                ov->bat.last_update_ms = now;
                break;
            }
            case VICTRON_BLE_RECORD_SOLAR_CHARGER: {
                const victron_record_solar_charger_t *s = &data->record.solar;
                ov->solar.has_data = true;
                ov->solar.state = s->device_state;
                ov->solar.pv_w = s->pv_power_w;
                ov->solar.load_current_deci = s->load_current_deci;
                ov->solar.voltage_centi = s->battery_voltage_centi;
                ov->solar.last_update_ms = now;
                energy_today_on_solar_yield(s->yield_today_centikwh);
                /* Si no hay BMV, también usamos los datos de batería del Solar */
                if (!ov->bat.has_data) {
                    ov->bat.has_data = true;
                    ov->bat.voltage_centi = s->battery_voltage_centi;
                    ov->bat.current_milli = (int32_t)s->battery_current_deci * 100;
                    ov->bat.last_update_ms = now;
                }
                break;
            }
            default: break;
        }
    }

    overview_render(ov);
}

static void overview_render(ui_overview_view_t *ov)
{
    if (!ov) return;
    uint32_t now = now_ms();
    const uint32_t TIMEOUT_MS = 30000;
    /* La card DC/DC (Orion XS) usa un timeout mucho mas largo: el Orion
     * llega muy debil (rssi ~-96..-99 dBm) y anuncia de forma muy esporadica,
     * asi que con 30s la card parpadeaba a "--V". Aguantamos el ultimo Vin/Vout
     * 5 min para una lectura estable (el dato cambia despacio). */
    const uint32_t DCDC_TIMEOUT_MS = 300000;
    char buf[24];

    bool bat_fresh   = ov->bat.has_data &&
                       (now - ov->bat.last_update_ms) < TIMEOUT_MS;
    bool solar_fresh = ov->solar.has_data &&
                       (now - ov->solar.last_update_ms) < TIMEOUT_MS;

    /* ── Solar A (corriente de carga PV→batería) ──────────────── */
    /* En SmartSolar BLE, battery_current_deci está en décimas de A
     * sobre el solar.pv_w (potencia). Si no tenemos battery_current_deci
     * publicado en el state-store, derivamos A = W/V. */
    long solar_centi_a = -1;  /* en cA (centi-amperios) */
    if (solar_fresh && ov->solar.voltage_centi > 0) {
        /* I = P/V → centi-A = W * 10000 / voltage_centi */
        solar_centi_a = ((long)ov->solar.pv_w * 10000L) /
                        (long)ov->solar.voltage_centi;
    }
    if (solar_centi_a >= 0) {
        /* UN DECIMAL (06-nov-2026, pedido del dueño). Antes se enseñaban dos
         * ("12.40") y el segundo decimal no aporta nada al que mira: el MPPT
         * no tiene esa precision y en la tarjeta solo servia para apretar la
         * cifra contra la etiqueta. Se redondea, no se trunca. */
        long ca = (solar_centi_a + 5);
        snprintf(buf, sizeof(buf), "%ld.%ld", ca / 100, (ca / 10) % 10);
        ui_metric_set(ov->m_solar_w, buf, "A",
                      solar_centi_a > 0 ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_solar_w, "--", "A", UI_COLOR_TEXT_DIM);
    }
    /* Pastilla de estado MPPT (pulsable para ver su explicacion). */
    if (solar_fresh)
        ui_pill_set(ov->pill_solar_state, ov_state_name(ov->solar.state),
                    ov_state_color(ov->solar.state));
    else
        ui_pill_set(ov->pill_solar_state, "-", UI_COLOR_TEXT_DIM);

    /* (Las flechas Solar→Bat / Bat→DC se eliminaron al pasar a layout
     * de 3 columnas verticales. El flujo se infiere de los colores.) */

    /* ── Batería: SOC + corriente ──────────────────────────────── */
    if (bat_fresh) {
        ui_battery_ring_set(ov->arc_soc, ov->bat.soc_deci);
        int32_t mi = ov->bat.current_milli;
        /* un decimal y redondeando: 12.55 A -> "12.6" */
        ui_fmt_dec1_signo(buf, sizeof(buf), mi >= 0 ? (mi + 5) / 10 : (mi - 5) / 10);
        ui_metric_set(ov->m_bat_current, buf, "A", ui_color_for_current(mi));
    } else {
        ui_battery_ring_set(ov->arc_soc, 0xFFFF);
        ui_metric_set(ov->m_bat_current, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── DC/DC (Orion-Tr Smart): muestra V_in (motor) y V_out (servicio) ── */
    bool dcdc_fresh = ov->dcdc.has_data &&
                      (now - ov->dcdc.last_update_ms) < DCDC_TIMEOUT_MS;
    if (dcdc_fresh) {
        /* V_in (batería motor) — métrica principal grande */
        ui_fmt_dec1(buf, sizeof(buf), (long)ov->dcdc.vin_centi);
        bool active = ov->dcdc.state != 0;
        ui_metric_set(ov->m_loads_w, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
        /* V_out (batería servicio). Si esta a 0 V no ponemos "APAGADO": la
         * pastilla de estado ya indica que el convertidor esta apagado. */
        ui_fmt_dec1(buf, sizeof(buf), (long)ov->dcdc.vout_centi);
        ui_metric_set(ov->m_loads_kwh, buf, "V",
                      active ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
    } else {
        ui_metric_set(ov->m_loads_w,   "--", "V", UI_COLOR_TEXT_DIM);
        ui_metric_set(ov->m_loads_kwh, "--", "V", UI_COLOR_TEXT_DIM);
    }
    /* Pastilla de estado Orion (mismos criterios que el MPPT; pulsable). */
    if (dcdc_fresh)
        ui_pill_set(ov->pill_dcdc_state, ov_state_name(ov->dcdc.state),
                    ov_state_color(ov->dcdc.state));
    else
        ui_pill_set(ov->pill_dcdc_state, "-", UI_COLOR_TEXT_DIM);

    /* ── TTG ──────────────────────────────────────────────────── */
    if (bat_fresh && (uint16_t)ov->bat.ttg_min != 0xFFFF && ov->bat.ttg_min > 0) {
        if (ov->bat.ttg_min >= 1440) {
            /* ≥ 24h → días y horas */
            snprintf(buf, sizeof(buf), "%ud %uh",
                     (unsigned)(ov->bat.ttg_min / 1440),
                     (unsigned)((ov->bat.ttg_min % 1440) / 60));
        } else if (ov->bat.ttg_min >= 60) {
            snprintf(buf, sizeof(buf), "%uh %02um",
                     (unsigned)(ov->bat.ttg_min / 60),
                     (unsigned)(ov->bat.ttg_min % 60));
        } else {
            snprintf(buf, sizeof(buf), "%um", (unsigned)ov->bat.ttg_min);
        }
        ui_metric_set(ov->m_ttg, buf, "", UI_COLOR_TEXT);
    } else {
        ui_metric_set(ov->m_ttg, "--", "", UI_COLOR_TEXT_DIM);
    }

    /* ── Camper (NE185 vía UART) ─────────────────────────────── */
    {
        ne185_data_t cd;
        ne185_get(&cd);

        /* Tanques visuales: nivel 0..3 (ui_tank_set ignora valores > 3).
         * SOLO SI CAMBIA EL NIVEL (auditoria del 27-oct-2026, O1): ui_tank_set
         * recorre los LEDs y les escribe el color, y cada escritura invalida.
         * Un tanque que no se mueve no tiene por que repintarse dos veces por
         * segundo. El "sin dato" (0xFF) cuenta como un nivel mas. */
        {
            uint8_t n1 = cd.fresh ? cd.s1 : 0xFF;
            uint8_t r1 = cd.fresh ? cd.r1 : 0xFF;
            if (ov->tank_s1 && n1 != s_ov_last_s1) { ui_tank_set(ov->tank_s1, n1); s_ov_last_s1 = n1; }
            if (ov->tank_r1 && r1 != s_ov_last_r1) { ui_tank_set(ov->tank_r1, r1); s_ov_last_r1 = r1; }
        }

        /* ── Alarmas ───────────────────────────────────────────
         * El estado viene de alarma_estado.c, que es quien temporiza y quien
         * pita (ver la cabecera del bloque de arriba). Aqui solo se lee: asi la
         * pantalla, la telemetria UDP y la orden que llega de la cabina dicen
         * siempre lo mismo, y el pitido deja de depender de que esta vista se
         * este dibujando. */
        bool alarm_s1  = alarma_activa(ALARMA_AGUA);
        bool alarm_r1  = alarma_activa(ALARMA_GRISES);
        bool alarm_soc = alarma_activa(ALARMA_BATERIA);

        ov->blink_phase ^= 1;

        /* Parpadeo visual: alternar opacidad cada 500 ms si la alarma esta
         * activa. SIGUE aunque este silenciada: lo que se calla es el pitido,
         * no la senal visual (decision del 13-sep-2026, y por eso el parpadeo
         * no mira alarma_silenciada()). */
        if (ov->tank_s1) {
            lv_opa_t opa = (alarm_s1 && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            lv_obj_set_style_opa(ov->tank_s1, opa, 0);
        }
        if (ov->tank_r1) {
            lv_opa_t opa = (alarm_r1 && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            lv_obj_set_style_opa(ov->tank_r1, opa, 0);
        }

        /* Aviso flotante e interrupcion del salvapantallas: viven en
         * alarma_estado.c, que evalua las cuatro alarmas en su propio timer
         * de 500 ms (independiente de que esta vista se dibuje). */
        /* Parpadeo visual del card de bateria.
         * SOLO SI CAMBIA (auditoria del 27-oct-2026, O1): lv_obj_set_style_opa
         * invalida siempre, y con la alarma apagada el valor es siempre el
         * mismo -- se repintaba la tarjeta dos veces por segundo para nada. */
        if (ov->card_bat) {
            lv_opa_t opa = (alarm_soc && ov->blink_phase)
                ? LV_OPA_30 : LV_OPA_COVER;
            if (lv_obj_get_style_opa(ov->card_bat, 0) != opa)
                lv_obj_set_style_opa(ov->card_bat, opa, 0);
        }

        /* Icono del altavoz en la esquina de cada tarjeta en alarma (ver
         * refresh_mute_iconos): se enseña solo si esa alarma esta activa, y
         * cambia de dibujo segun este sonando o silenciada. */
        refresh_mute_iconos(ov);

        /* Indicador 230 V grande: la pastilla se pinta como siempre (verde si
         * hay 230 V) y ademas su LED de la derecha, igual que el de los botones
         * (5-oct-2026: "230 V con LED a la derecha"). */
        if (ov->pill_shore) {
            lv_obj_set_style_bg_color(ov->pill_shore,
                cd.fresh && cd.shore ? UI_COLOR_GREEN : UI_COLOR_CARD_LO, 0);
        }
        /* El texto "230 V" se mantiene fijo; solo cambia el color del pill */
        /* Estetica del boton se mantiene igual ON/OFF (bg gris, texto
         * del color del acento). Unico indicador de estado: el LED
         * verde con halo cuando ON. */
        #define UPDATE_CAMPER_BTN(btn, on, color_on) do {                   \
            if (!(btn)) break;                                              \
            (void)(color_on);                                               \
            camper_led_paint((lv_obj_t *)lv_obj_get_user_data(btn), (on));  \
        } while (0)

        UPDATE_CAMPER_BTN(ov->btn_lin,  cd.fresh && cd.light_in,  UI_COLOR_YELLOW);
        UPDATE_CAMPER_BTN(ov->btn_lout, cd.fresh && cd.light_out, UI_COLOR_YELLOW);
        UPDATE_CAMPER_BTN(ov->btn_pump, cd.fresh && cd.pump,      UI_COLOR_CYAN);
        #undef UPDATE_CAMPER_BTN
    }

    /* ── Frigo: T_Congelador y % ventilador ─────────────────── */
    {
        frigo_state_t fs_copy;
        frigo_get_state_copy(&fs_copy);
        const frigo_state_t *fs = &fs_copy;
        /* Historial corto para la flecha de tendencia: una muestra por
         * minuto, 15 minutos. Vive aquí (RAM), no en la SD: la tarjeta se
         * refresca dos veces por segundo y leer el CSV tarda segundos. */
        #define OV_TREND_N 15
        static float  s_tc_hist[OV_TREND_N];
        static int    s_tc_n;
        static uint32_t s_tc_next_ms;
        if (fs && fs->T_Congelador > -120.0f) {
            uint32_t tnow = now_ms();
            if (s_tc_next_ms == 0 || tnow >= s_tc_next_ms) {
                s_tc_next_ms = tnow + 60000;
                if (s_tc_n < OV_TREND_N) {
                    s_tc_hist[s_tc_n++] = fs->T_Congelador;
                } else {
                    memmove(s_tc_hist, s_tc_hist + 1,
                            sizeof(float) * (OV_TREND_N - 1));
                    s_tc_hist[OV_TREND_N - 1] = fs->T_Congelador;
                }
            }
        }
        if (fs && ov->lbl_freezer_temp) {
            char fbuf[16];
            bool over = alarma_activa(ALARMA_CONGELADOR);
            lv_color_t col;
            lv_opa_t opa;
            if (fs->T_Congelador > -120.0f) {
                /* Reservar siempre la columna del signo: con el numero
                 * centrado, sin esto "salta" lateralmente al pasar de
                 * positivo (sin '-') a negativo (con '-'). Para positivos
                 * anteponemos un espacio para igualar el ancho. La unidad
                 * "C" va aparte (label propio en fuente _es con glifo grado). */
                float t = fs->T_Congelador;
                if (t < 0.0f)
                    snprintf(fbuf, sizeof(fbuf), "-%.1f", -t);
                else
                    snprintf(fbuf, sizeof(fbuf), " %.1f", t);
                /* Color: rojo si supera umbral, blanco si OK.
                 * Parpadeo (alfa 30% / 100%) si la alarma esta activa, para
                 * que destaque sobre la barra inferior. El parpadeo NO mira si
                 * esta silenciada: lo que se calla es el pitido. */
                col = over ? UI_COLOR_RED : UI_COLOR_TEXT;
                opa = (over && ov->blink_phase)
                    ? LV_OPA_30 : LV_OPA_COVER;
            } else {
                snprintf(fbuf, sizeof(fbuf), " --");
                col = UI_COLOR_TEXT_DIM;
                opa = LV_OPA_COVER;
            }
            lv_label_set_text(ov->lbl_freezer_temp, fbuf);
            /* Mismo color y parpadeo en numero y unidad para que vayan a una. */
            lv_obj_set_style_text_color(ov->lbl_freezer_temp, col, 0);
            lv_obj_set_style_text_opa(ov->lbl_freezer_temp, opa, 0);
            if (ov->lbl_freezer_unit) {
                lv_obj_set_style_text_color(ov->lbl_freezer_unit, col, 0);
                lv_obj_set_style_text_opa(ov->lbl_freezer_unit, opa, 0);
            }
            if (ov->ln_freezer_trend) {
                /* Diferencia entre la muestra más reciente y la más vieja del
                 * historial (15 min como mucho). 0.15 °C de histéresis para
                 * que no parpadee con el ruido del sensor. */
                static const lv_point_t s_arriba[] = { {0, 8}, {6, 0}, {12, 8} };
                static const lv_point_t s_abajo[]  = { {0, 0}, {6, 8}, {12, 0} };
                float d = 0.0f;
                if (s_tc_n >= 2) d = s_tc_hist[s_tc_n - 1] - s_tc_hist[0];
                if (d <= -0.15f) {
                    lv_line_set_points(ov->ln_freezer_trend, s_abajo, 3);
                    lv_obj_set_style_line_color(ov->ln_freezer_trend,
                                                UI_COLOR_CYAN, 0);
                    lv_obj_clear_flag(ov->ln_freezer_trend, LV_OBJ_FLAG_HIDDEN);
                } else if (d >= 0.15f) {
                    lv_line_set_points(ov->ln_freezer_trend, s_arriba, 3);
                    lv_obj_set_style_line_color(ov->ln_freezer_trend,
                                                UI_COLOR_ORANGE, 0);
                    lv_obj_clear_flag(ov->ln_freezer_trend, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ov->ln_freezer_trend, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        if (fs && ov->arc_fan) {
            uint8_t p = fs->fan_percent;
            if (p > 100) p = 100;
            lv_arc_set_value(ov->arc_fan, p);
            /* El ARO es el indicador de velocidad: se llena segun el porcentaje
             * y su color va con el (gris 0x8A93A6 al 0 % -> naranja 0xFF9800 al
             * 50 % -> rojo 0xFF4444 al 100 %), como antes del 15-sep-2026. */
            uint8_t r, g, b;
            if (p < 50) {
                r = 0x8A + ((int)(0xFF - 0x8A) * p) / 50;
                g = 0x93 + ((int)(0x98 - 0x93) * p) / 50;
                b = 0xA6 + ((int)(0x00 - 0xA6) * p) / 50;
            } else {
                int q = p - 50;
                r = 0xFF;
                g = 0x98 + ((int)(0x44 - 0x98) * q) / 50;
                b = 0x00 + ((int)(0x44 - 0x00) * q) / 50;
            }
            lv_obj_set_style_arc_color(ov->arc_fan, lv_color_make(r, g, b),
                                       LV_PART_INDICATOR);
            /* El chivato del excedente solar son las ASPAS del ventilador
             * (peticion del usuario, 15-sep-2026): verdes intensas cuando el
             * ventilador esta tirando AHORA del sol que sobra, y gris apagado
             * cuando no (la espera de excedente ya la dice el cartel ambar de
             * la tarjeta). Antes las aspas eran siempre grises. */
            /* Y ADEMAS DEL INTERRUPTOR (08-nov-2026): verde solo si el modo
             * esta puesto Y esta tirando del sol AHORA. Con el interruptor
             * fuera, gris, aunque la maquina tarde una vuelta en enterarse. */
            lv_obj_set_style_img_recolor(ov->img_fan,
                (frigo_solar_get_enabled() && frigo_solar_get_active())
                    ? UI_COLOR_GREEN
                    : UI_COLOR_TEXT_DIM, 0);
        }
    }

    /* ── Energía solar acumulada del día (kWh hoy en card Solar) ── */
    {
        float pv = energy_today_pv_kwh();
        bool fresh_today = energy_today_is_fresh();
        char ebuf[16];
        if (ov->m_solar_kwh) {
            if (pv > 0.0f || fresh_today) {
                snprintf(ebuf, sizeof(ebuf), "%.1f", pv);   /* un decimal (06-nov) */
                ui_metric_set(ov->m_solar_kwh, ebuf, "kWh",
                              pv > 0.0f ? UI_COLOR_GREEN : UI_COLOR_TEXT_DIM);
            } else {
                ui_metric_set(ov->m_solar_kwh, "--", "kWh", UI_COLOR_TEXT_DIM);
            }
        }
        /* card_dcdc: m_loads_kwh ya lo gestiona la rama DC/DC arriba con
         * V_out (servicio), no kWh. No tocar aquí. */
    }
}

/* Una sola vez, con el layout ya resuelto: baja el indicador de aguas grises
 * (con su titulo) para que su base coincida con la base del nivel de aguas
 * limpias. El translate_y es solo visual: no mueve el 230V ni nada mas, y al
 * estar en el flujo el ancho de la columna no cambia (el texto no se recorta).
 * OJO: aqui se le forzaba el ANCHO al del pill de 230V (era un rectangulo liso).
 * Desde que es un ICONO dibujado (v4.31) eso lo estiraba a 119 px y descuadraba
 * el dibujo por dentro: las ondas se iban a la izquierda (visto el 6-oct-2026).
 * El icono tiene su medida (76) y se centra solo. */
static void overview_align_grey(ui_overview_view_t *ov)
{
    if (!ov || ov->grey_aligned) return;
    if (!ov->tank_r1 || !ov->tank_s1 || !ov->pill_shore) return;

    lv_obj_update_layout(ov->base.root);
    lv_coord_t pill_w = lv_obj_get_width(ov->pill_shore);
    if (pill_w < 24) return;  /* layout aun no resuelto: reintenta en otro show */

    lv_obj_t *grey_body  = lv_obj_get_child(ov->tank_r1, 1);
    lv_obj_t *clean_body = lv_obj_get_child(ov->tank_s1, 1);
    if (!grey_body || !clean_body) return;
    (void)pill_w;   /* ya no se usa: el icono NO se estira al ancho del 230V */

    /* Bajar el tanque gris hasta igualar su base con la del nivel limpio...
     * PERO sin salirse de la tarjeta. El 4-oct-2026, al subir los rellenos a la
     * rejilla de 4 px (14 -> 16), el nivel limpio quedo mas abajo y este
     * desplazamiento crecio tanto que el rectangulo gris se salia por el borde
     * inferior de la tarjeta y salia CORTADO. Se acota: como mucho hasta dejar
     * UI_PAD_8 de aire sobre el borde inferior de la tarjeta que lo contiene. */
    lv_area_t ca, ga;
    lv_obj_get_coords(clean_body, &ca);
    lv_obj_get_coords(grey_body, &ga);
    lv_coord_t delta = ca.y2 - ga.y2;
    lv_obj_t *card = lv_obj_get_parent(ov->tank_s1);   /* la tarjeta camper: es la
                                                        * que RECORTA (el padre
                                                        * inmediato del gris es la
                                                        * columna, que no recorta) */
    lv_area_t cc = {0};
    if (card) lv_obj_get_coords(card, &cc);
    lv_obj_t *col = lv_obj_get_parent(ov->tank_r1);   /* la columna: es la que
                                                       * RECORTA el indicador */
    lv_area_t lc = {0};
    if (col) lv_obj_get_coords(col, &lc);
    const lv_coord_t tope_card = card ? (cc.y2 - UI_PAD_8 - ga.y2) : delta;
    /* Aire tambien respecto a la columna: si la base del indicador queda justo
     * en el borde, el recorte se come su borde inferior de 4 px (es lo que se
     * veia: el rectangulo "sin suelo"). */
    const lv_coord_t tope_col  = col  ? (lc.y2 - UI_PAD_8 - ga.y2) : delta;
    lv_coord_t tope = (tope_card < tope_col) ? tope_card : tope_col;
    if (delta > tope) delta = tope;
    if (delta > 0) lv_obj_set_style_translate_y(ov->tank_r1, delta, 0);
    ov->grey_aligned = true;
}

static void overview_show(ui_device_view_t *view)
{
    if (view && view->root) {
        lv_obj_clear_flag(view->root, LV_OBJ_FLAG_HIDDEN);
        overview_align_grey((ui_overview_view_t *)view);
    }
}

static void overview_hide(ui_device_view_t *view)
{
    if (view && view->root) lv_obj_add_flag(view->root, LV_OBJ_FLAG_HIDDEN);
}

static void overview_destroy(ui_device_view_t *view)
{
    if (!view) return;
    ui_overview_view_t *ov = (ui_overview_view_t *)view;
    if (ov->camper_tick_timer)   { lv_timer_del(ov->camper_tick_timer);   ov->camper_tick_timer   = NULL; }
    if (ov->fan_rotate_timer)    { lv_timer_del(ov->fan_rotate_timer);    ov->fan_rotate_timer    = NULL; }
    /* El temporizador de alarmas NO se borra aqui a proposito: no es de esta
     * vista, vive en alarma_estado.c y tiene que seguir evaluando (y pitando)
     * con la vista fuera. Si no, destruir la vista dejaria las alarmas mudas. */
    /* El aviso de estado tampoco cuelga del root (va en lv_layer_top), asi que
     * hay que borrarlo expresamente: si no, se quedaria flotando encima. */
    if (ov->info_modal) { lv_obj_del(ov->info_modal); ov->info_modal = NULL; }
    if (view->root) { lv_obj_del(view->root); view->root = NULL; }
    free(view);
}
