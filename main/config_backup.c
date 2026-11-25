#include "config_backup.h"
#include "config_storage.h"
#include "alerts.h"
#include "frigo.h"
#include "cJSON.h"

#include <stdio.h>
#include <unistd.h>   /* unlink(): borrar el respaldo a medias */
#include <string.h>
#include <strings.h>   /* strcasecmp: casar MACs al recuperar la clave AES de NVS */
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "camera.h"   /* camera_sd_bus_lock: serializar SD con la camara */

static const char *TAG = "CFG_BAK";

/* hex_encode retirado: era solo para exportar las claves AES, que ya no salen
 * del aparato. hex_decode se conserva para leer backups antiguos. */

static int hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) < n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        char a = hex[i * 2], b = hex[i * 2 + 1];
        int va = (a >= '0' && a <= '9') ? a - '0'
               : (a >= 'a' && a <= 'f') ? a - 'a' + 10
               : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        int vb = (b >= '0' && b <= '9') ? b - '0'
               : (b >= 'a' && b <= 'f') ? b - 'a' + 10
               : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if (va < 0 || vb < 0) return -1;
        out[i] = (uint8_t)((va << 4) | vb);
    }
    return 0;
}

esp_err_t config_backup_export(const char *path)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);

    /* Display */
    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    uint8_t bri = 80;
    load_brightness(&bri);
    cJSON_AddNumberToObject(disp, "brightness", bri);

    uint8_t view_mode = 1;
    load_ui_view_mode(&view_mode);
    cJSON_AddNumberToObject(disp, "view_mode", view_mode);

    char tz[48] = {0};
    load_timezone(tz, sizeof(tz));
    cJSON_AddStringToObject(disp, "timezone", tz);

    /* Night mode */
    bool nm_en = false;
    uint8_t nm_s = 22, nm_e = 7;
    load_night_mode(&nm_en, &nm_s, &nm_e);
    cJSON *nm = cJSON_AddObjectToObject(disp, "night_mode");
    cJSON_AddBoolToObject(nm, "enabled", nm_en);
    cJSON_AddNumberToObject(nm, "start_h", nm_s);
    cJSON_AddNumberToObject(nm, "end_h", nm_e);

    /* Screensaver */
    bool ss_en = false;
    uint8_t ss_bri = 30;
    uint16_t ss_to = 60;
    load_screensaver_settings(&ss_en, &ss_bri, &ss_to);
    uint8_t ss_mode = 0, ss_period = 5;
    load_screensaver_mode(&ss_mode, &ss_period);
    cJSON *ss = cJSON_AddObjectToObject(disp, "screensaver");
    cJSON_AddBoolToObject(ss, "enabled", ss_en);
    cJSON_AddNumberToObject(ss, "brightness", ss_bri);
    cJSON_AddNumberToObject(ss, "timeout_s", ss_to);
    cJSON_AddNumberToObject(ss, "mode", ss_mode);
    cJSON_AddNumberToObject(ss, "rotate_period_min", ss_period);

    /* Alerts */
    cJSON *al = cJSON_AddObjectToObject(root, "alerts");
    cJSON_AddNumberToObject(al, "freezer_minutes", alerts_get_freezer_minutes());
    cJSON_AddNumberToObject(al, "freezer_temp_c", alerts_get_freezer_temp_c());
    cJSON_AddNumberToObject(al, "soc_critical", alerts_get_soc_critical());
    cJSON_AddNumberToObject(al, "soc_warning", alerts_get_soc_warning());

    /* Wi-Fi (sin password por seguridad) */
    char ssid[33] = {0}, pass[65] = {0};
    size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
    uint8_t wifi_en = 0;
    load_wifi_config(ssid, &ssid_len, pass, &pass_len, &wifi_en);
    cJSON *wf = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wf, "ssid", ssid);
    cJSON_AddBoolToObject(wf, "enabled", wifi_en != 0);

    /* Victron devices.
     * Las claves AES NO se exportan, mismo criterio que el password del Wi-Fi:
     * este JSON vive en la SD y se sirve entero por HTTP en /data/config.tar,
     * asi que exportarlas en claro las sacaba del aparato por la red. Al
     * importar se conserva la clave que ya hubiera en NVS para esa MAC (ver
     * config_backup_import), asi que un ciclo exportar/importar en el mismo
     * equipo no pierde nada. */
    cJSON *vict = cJSON_AddArrayToObject(root, "victron_devices");
    victron_device_config_t devs[VICTRON_MAX_DEVICES];
    uint8_t count = 0;
    load_victron_devices(devs, &count, VICTRON_MAX_DEVICES);
    for (uint8_t i = 0; i < count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "mac", devs[i].mac_address);
        cJSON_AddStringToObject(d, "name", devs[i].device_name);
        cJSON_AddBoolToObject(d, "enabled", devs[i].enabled);
        cJSON_AddItemToArray(vict, d);
    }

    /* Frigo: SOLO los ajustes de mando (umbrales del ventilador, suelo y SoC
     * del modo solar). NO se exportan la asignacion de sondas ni sus
     * direcciones: van atadas al cableado fisico de esta furgo y restaurarlas
     * en otra pantalla seria peor que no restaurar nada (19-nov-2026).
     * Asi "restaurar la configuracion" de verdad deja el frigo como estaba y no
     * de fabrica. */
    frigo_state_t fr;
    frigo_get_state_copy(&fr);
    cJSON *frigo = cJSON_AddObjectToObject(root, "frigo");
    cJSON_AddNumberToObject(frigo, "t_min", fr.T_min);
    cJSON_AddNumberToObject(frigo, "t_max", fr.T_max);
    cJSON_AddNumberToObject(frigo, "fan_min_pct", fr.fan_min_pct);
    cJSON_AddBoolToObject(frigo, "sol_enabled", frigo_solar_get_enabled());
    cJSON_AddNumberToObject(frigo, "sol_soc_on", frigo_solar_get_soc_on());
    cJSON_AddNumberToObject(frigo, "sol_soc_off", frigo_solar_get_soc_off());

    /* Victron debug */
    bool vdbg = false;
    load_victron_debug(&vdbg);
    cJSON_AddBoolToObject(root, "victron_debug", vdbg);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    /* Sin cerrojo no se toca la SD: escribir pisando el GDMA de la camara es lo
     * que acaba en "DMA timeout" y tarjeta pillada. 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) {
        free(json);
        ESP_LOGW(TAG, "tarjeta ocupada por la camara: no se exporta");
        return ESP_ERR_TIMEOUT;
    }
    FILE *fp = fopen(path, "w");
    if (!fp) { camera_sd_bus_unlock(); free(json); ESP_LOGE(TAG, "fopen %s", path); return ESP_FAIL; }
    size_t len = strlen(json);
    size_t wrote = fwrite(json, 1, len, fp);
    /* EL fclose SE MIRA (auditoria del 25-nov-2026, H-25): con stdio el error real
     * (tarjeta llena, extraida, E/S que no responde) sale al cerrar, no en el
     * fwrite. Antes se decia "Exportado" en verde con el fichero a medias -- y
     * como se abre con "w", el respaldo ANTERIOR ya estaba truncado. Mismo
     * criterio que ya usa log_capture.c. */
    const int cierre = fclose(fp);
    if (cierre != 0 || wrote != len) {
        ESP_LOGE(TAG, "NO se ha exportado: %s (%u de %u bytes, fclose=%d)",
                 path, (unsigned)wrote, (unsigned)len, cierre);
        if (camera_sd_bus_lock(2000)) {
            unlink(path);          /* que no quede un respaldo a medias */
            camera_sd_bus_unlock();
        }
        camera_sd_bus_unlock();
        free(json);
        return ESP_FAIL;
    }
    camera_sd_bus_unlock();
    free(json);
    ESP_LOGI(TAG, "Exportado: %s (%u bytes)", path, (unsigned)wrote);
    return ESP_OK;
}

/* Valor entero de un campo, COMPROBANDO EL RANGO (auditoria del 25-nov-2026,
 * H-16). Antes se hacia (uint8_t)v->valueint sin mirar: un 286 se convierte en 30
 * y se aplica en silencio -- el mismo fallo que este fichero ya corrigio para el
 * frigo. Si el valor no esta entre min y max, se deja el defecto. */
static int campo_rango(const cJSON *obj, const char *clave, int min, int max, int def)
{
    const cJSON *v = obj ? cJSON_GetObjectItem(obj, clave) : NULL;
    if (!v || !cJSON_IsNumber(v)) return def;
    int n = v->valueint;
    if (n < min) n = min;
    if (n > max) n = max;
    return n;
}

esp_err_t config_backup_import(const char *path)
{
    /* Sin cerrojo no se toca la SD (ver config_backup_export). 2026-07-26. */
    if (!camera_sd_bus_lock(3000)) {
        ESP_LOGW(TAG, "tarjeta ocupada por la camara: no se importa");
        return ESP_ERR_TIMEOUT;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) { camera_sd_bus_unlock(); ESP_LOGE(TAG, "No se puede abrir %s", path); return ESP_FAIL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024) { fclose(fp); camera_sd_bus_unlock(); return ESP_FAIL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); camera_sd_bus_unlock(); return ESP_ERR_NO_MEM; }
    /* EL fread SE COMPRUEBA (auditoria del 25-nov-2026, H-16): si la lectura es
     * corta, lo que queda hasta buf[sz] es heap sin inicializar y cJSON lo
     * interpretaba como JSON. Se exige la lectura entera. */
    const size_t leido = fread(buf, 1, (size_t)sz, fp);
    const bool corto = (leido != (size_t)sz) || (ferror(fp) != 0);
    fclose(fp);
    if (corto) {
        camera_sd_bus_unlock();
        free(buf);
        ESP_LOGE(TAG, "no he podido leer %s entero (%u de %ld): no importo nada",
                 path, (unsigned)leido, sz);
        return ESP_FAIL;
    }
    buf[sz] = 0;
    camera_sd_bus_unlock();

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGE(TAG, "JSON invalido"); return ESP_FAIL; }

    /* Display */
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (disp) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(disp, "brightness")) && cJSON_IsNumber(v))
            save_brightness((uint8_t)campo_rango(disp, "brightness", 5, 100, 80));
        if ((v = cJSON_GetObjectItem(disp, "view_mode")) && cJSON_IsNumber(v))
            save_ui_view_mode((uint8_t)campo_rango(disp, "view_mode", 0, 1, 0));
        if ((v = cJSON_GetObjectItem(disp, "timezone")) && cJSON_IsString(v))
            save_timezone(v->valuestring);

        cJSON *nm = cJSON_GetObjectItem(disp, "night_mode");
        if (nm) {
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(nm, "enabled"));
            int s = campo_rango(nm, "start_h", 0, 23, 22);
            int e = campo_rango(nm, "end_h",   0, 23, 7);
            save_night_mode(en, (uint8_t)s, (uint8_t)e);
        }
        cJSON *ss = cJSON_GetObjectItem(disp, "screensaver");
        if (ss) {
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(ss, "enabled"));
            int b  = campo_rango(ss, "brightness", 0, 100, 30);
            int to = campo_rango(ss, "timeout_s", 5, 3600, 60);
            int md = campo_rango(ss, "mode", 0, 5, 0);
            int pe = campo_rango(ss, "rotate_period_min", 1, 240, 5);
            save_screensaver_settings(en, (uint8_t)b, (uint16_t)to);
            save_screensaver_mode((uint8_t)md, (uint8_t)pe);
        }
    }

    cJSON *al = cJSON_GetObjectItem(root, "alerts");
    if (al) {
        cJSON *v;
        /* clamp: backup corrupto no debe dejar umbrales absurdos/negativos */
        if ((v = cJSON_GetObjectItem(al, "freezer_minutes")) && cJSON_IsNumber(v)) {
            int m = v->valueint;
            if (m < 0) m = 0;
            if (m > 1440) m = 1440;
            alerts_set_freezer_minutes(m);
        }
        if ((v = cJSON_GetObjectItem(al, "freezer_temp_c")) && cJSON_IsNumber(v)) {
            float t = (float)v->valuedouble;
            if (t < -30.0f) t = -30.0f;
            if (t > 10.0f) t = 10.0f;
            alerts_set_freezer_temp_c(t);
        }
        if ((v = cJSON_GetObjectItem(al, "soc_critical")) && cJSON_IsNumber(v)) {
            int p = v->valueint;
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            alerts_set_soc_critical(p);
        }
        if ((v = cJSON_GetObjectItem(al, "soc_warning")) && cJSON_IsNumber(v)) {
            int p = v->valueint;
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            alerts_set_soc_warning(p);
        }
    }

    /* Frigo (19-nov-2026). Se aplica con los setters de siempre, que validan
     * rango y orden; un valor absurdo en el fichero no se aplica y no rompe el
     * resto de la importacion. Los backup antiguos no traen este bloque: se
     * saltan sin mas. */
    cJSON *fr = cJSON_GetObjectItem(root, "frigo");
    if (cJSON_IsObject(fr)) {
        /* Cada valor se aplica SOLO si esta y es del tipo correcto; los setters
         * vuelven a validar el rango y el orden, asi que un fichero retocado a
         * mano no puede dejar el frigo en un estado imposible. */
        /* OJO: el cast a uint8_t va DESPUES de comprobar el rango del JSON. Si no,
         * un 286 del fichero se convierte en 30 y se aplica en silencio: el
         * estado queda valido pero no es el del fichero (auditoria 19-nov-2026).
         * Rango util: 0..255, que es lo que cabe en el tipo. */
        cJSON *v_tmin = cJSON_GetObjectItem(fr, "t_min");
        cJSON *v_tmax = cJSON_GetObjectItem(fr, "t_max");
/* Numero entero y en 0..255: ademas de no desbordar el cast, exige que el
 * valor sea ENTERO (un 45.9 del fichero no se trunca a 45 en silencio). */
#define VALOR_U8_OK(v) (cJSON_IsNumber(v) && (v)->valueint >= 0 && \
                        (v)->valueint <= 255 && \
                        (double)(v)->valueint == (v)->valuedouble)
        if (VALOR_U8_OK(v_tmin) && VALOR_U8_OK(v_tmax)) {
            esp_err_t e = frigo_set_thresholds((uint8_t)v_tmin->valueint,
                                               (uint8_t)v_tmax->valueint);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "frigo: umbrales %d/%d rechazados (%s), se quedan como estan",
                         v_tmin->valueint, v_tmax->valueint, esp_err_to_name(e));
            }
        }
        cJSON *v_fmin = cJSON_GetObjectItem(fr, "fan_min_pct");
        if (VALOR_U8_OK(v_fmin)) {
            esp_err_t e = frigo_set_fan_min((uint8_t)v_fmin->valueint);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "frigo: suelo %d%% rechazado (%s)",
                         v_fmin->valueint, esp_err_to_name(e));
            }
        }
        /* El SoC de activacion primero: el de corte se valida contra el. */
        cJSON *v_son  = cJSON_GetObjectItem(fr, "sol_soc_on");
        cJSON *v_soff = cJSON_GetObjectItem(fr, "sol_soc_off");
        if (VALOR_U8_OK(v_son)) {
            esp_err_t e = frigo_solar_set_soc_on((uint8_t)v_son->valueint);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "frigo: SoC de activacion %d%% rechazado (%s)",
                         v_son->valueint, esp_err_to_name(e));
            }
        }
        if (VALOR_U8_OK(v_soff)) {
            esp_err_t e = frigo_solar_set_soc_off((uint8_t)v_soff->valueint);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "frigo: SoC de corte %d%% rechazado (%s)",
                         v_soff->valueint, esp_err_to_name(e));
            }
        }
        cJSON *v_en = cJSON_GetObjectItem(fr, "sol_enabled");
        if (cJSON_IsBool(v_en)) {
            esp_err_t e = frigo_solar_set_enabled(cJSON_IsTrue(v_en));
            if (e != ESP_OK) ESP_LOGW(TAG, "frigo: interruptor solar no aplicado (%s)",
                                      esp_err_to_name(e));
        }
#undef VALOR_U8_OK

        frigo_state_t fst;
        frigo_get_state_copy(&fst);
        ESP_LOGI(TAG, "frigo importado: umbrales %d/%d, suelo %d%%, sol %s %d/%d",
                 (int)fst.T_min, (int)fst.T_max, (int)fst.fan_min_pct,
                 frigo_solar_get_enabled() ? "on" : "off",
                 (int)frigo_solar_get_soc_on(), (int)frigo_solar_get_soc_off());
    }

    /* Victron devices: sustitución completa */

    cJSON *vict = cJSON_GetObjectItem(root, "victron_devices");
    if (cJSON_IsArray(vict)) {
        victron_device_config_t devs[VICTRON_MAX_DEVICES];
        memset(devs, 0, sizeof(devs));
        uint8_t count = 0;
        /* Config actual: el backup ya no lleva las claves AES (ver export), asi
         * que se recuperan de aqui casando por MAC. Los backups ANTIGUOS que si
         * las llevan siguen funcionando (tienen prioridad). */
        victron_device_config_t cur[VICTRON_MAX_DEVICES];
        uint8_t cur_count = 0;
        load_victron_devices(cur, &cur_count, VICTRON_MAX_DEVICES);
        cJSON *d;
        cJSON_ArrayForEach(d, vict) {
            if (count >= VICTRON_MAX_DEVICES) break;
            const char *mac  = cJSON_GetObjectItem(d, "mac") ? cJSON_GetObjectItem(d, "mac")->valuestring : NULL;
            const char *khex = cJSON_GetObjectItem(d, "aes_key_hex") ? cJSON_GetObjectItem(d, "aes_key_hex")->valuestring : NULL;
            cJSON *jname = cJSON_GetObjectItem(d, "name");
            const char *name = cJSON_IsString(jname) ? jname->valuestring : "";
            bool en = cJSON_IsTrue(cJSON_GetObjectItem(d, "enabled"));
            if (!mac) continue;
            strncpy(devs[count].mac_address, mac, sizeof(devs[count].mac_address) - 1);
            strncpy(devs[count].device_name, name, sizeof(devs[count].device_name) - 1);

            bool have_key = (khex && hex_decode(khex, devs[count].aes_key, 16) == 0);
            if (!have_key) {
                for (uint8_t k = 0; k < cur_count; k++) {
                    if (strcasecmp(cur[k].mac_address, devs[count].mac_address) == 0) {
                        memcpy(devs[count].aes_key, cur[k].aes_key, 16);
                        have_key = true;
                        break;
                    }
                }
            }
            /* Sin clave utilizable el dispositivo no sirve para nada: no lo
             * guardamos (evita dejar una entrada con clave a ceros). */
            if (!have_key) {
                ESP_LOGW(TAG, "Import: %s sin clave AES (ni en el backup ni en NVS), omitido", mac);
                memset(&devs[count], 0, sizeof(devs[count]));
                continue;
            }
            devs[count].enabled = en;
            count++;
        }
        save_victron_devices(devs, count);
    }

    cJSON *vd = cJSON_GetObjectItem(root, "victron_debug");
    if (vd) save_victron_debug(cJSON_IsTrue(vd));

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Importado desde %s", path);
    return ESP_OK;
}
