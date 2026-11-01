// config_storage.h
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "nvs.h"

/* DEFAULT_AP_PASSWORD ("12345678") eliminado 2026-07-26: la clave del AP la
 * genera aleatoria wifi_ap_init() y se lee en Ajustes -> Wi-Fi. Ya no hay
 * ninguna clave de fabrica en el codigo. */

// brightness settings
esp_err_t load_brightness(uint8_t *brightness_out);
esp_err_t save_brightness(uint8_t brightness);

// AES key handling
// Legacy single AES key handling (for compatibility)
esp_err_t load_aes_key(uint8_t key_out[16]);

// New multiple Victron device configuration
#define VICTRON_MAX_DEVICES 8

typedef struct {
    char mac_address[18];    // "XX:XX:XX:XX:XX:XX" format
    uint8_t aes_key[16];     // 16-byte AES key
    char device_name[32];    // User-friendly device name
    bool enabled;            // Whether this device is active
} victron_device_config_t;

esp_err_t load_victron_devices(victron_device_config_t *devices_out, 
                               uint8_t *count_out, 
                               uint8_t max_devices);
esp_err_t save_victron_devices(const victron_device_config_t *devices, 
                               uint8_t count);

// Helper function to add a single device
esp_err_t add_victron_device(const uint8_t mac[6], const uint8_t aes_key[16]);

// Screensaver settings
esp_err_t load_screensaver_settings(bool *enabled, uint8_t *brightness, uint16_t *timeout);
esp_err_t save_screensaver_settings(bool enabled, uint8_t brightness, uint16_t timeout);
esp_err_t load_screensaver_mode(uint8_t *mode_out, uint8_t *rotate_period_min_out);
esp_err_t save_screensaver_mode(uint8_t mode, uint8_t rotate_period_min);

/* NOMBRE DE FABRICA DEL AP (01-nov-2026).
 *
 * El proyecto ya no se llama "victron": eso es UNA parte de lo que lleva
 * dentro (la integracion Victron: BLE, productos, claves). La placa es la
 * P4, y las otras dos pantallas se llaman por lo que son (cabina5,
 * 35cabina). El nombre viejo era "VictronConfig".
 *
 * CAMBIO SIN DEJAR A NADIE TIRADO: esto solo es el valor de FABRICA, el que
 * se escribe en NVS cuando esa clave no existe. Una placa ya instalada
 * sigue anunciandose con el nombre que tenga guardado (todas: "VictronConfig")
 * hasta que se cambie a mano en Ajustes -> Wi-Fi. Eso se hace cuando las dos
 * cabinas lleven la version que prueba los dos nombres (ver WIFI_SSID_* en
 * el main/net/udp_rx.c de las cabinas), no antes: una cabina que no encuentre
 * el AP se queda sin telemetria ni OTA, y hay dos en la autocaravana. */
#define P4_SSID_FABRICA  "P4Config"

// Wi‑Fi AP settings handling (NVS namespace: "wifi")
// ssid_out and pass_out must have space for ssid_len and pass_len, respectively.
// On return, *ssid_len and *pass_len are set to the actual string lengths (including null).
esp_err_t load_wifi_config(char *ssid_out, size_t *ssid_len,
                           char *pass_out, size_t *pass_len,
                           uint8_t *enabled_out);

// Victron BLE debug flag persistence (NVS namespace: "debug")
esp_err_t load_victron_debug(bool *enabled_out);
esp_err_t save_victron_debug(bool enabled);

// Auto-encender cargas (luz int + bomba) al arranque del NE185, si estan
// apagadas. NVS namespace: "ne185". Default: deshabilitado (0).
esp_err_t load_autostart_loads(bool *enabled_out);
esp_err_t save_autostart_loads(bool enabled);

// UI view mode selection (NVS namespace: "display")
esp_err_t load_ui_view_mode(uint8_t *mode_out);
esp_err_t save_ui_view_mode(uint8_t mode);

// Splash screen al boot (NVS namespace: "display", key "splash")
// 0 = sin splash, 1 = logo furgo embebido. Default 1.
esp_err_t load_splash_mode(uint8_t *mode_out);
esp_err_t save_splash_mode(uint8_t mode);

// Zona horaria en formato POSIX TZ (NVS namespace: "display").
// Default: Madrid ("CET-1CEST,M3.5.0,M10.5.0/3").
esp_err_t load_timezone(char *tz_out, size_t maxlen);
esp_err_t save_timezone(const char *tz_str);

// Night mode (auto brightness by RTC hour). NVS namespace: "display"
// start_h / end_h en formato 0..23. Si start==end, la ventana es vacía.
// Si start>end, la ventana cruza la medianoche (ej. 22 → 7).
esp_err_t load_night_mode(bool *enabled_out,
                          uint8_t *start_h_out,
                          uint8_t *end_h_out);
esp_err_t save_night_mode(bool enabled,
                          uint8_t start_h,
                          uint8_t end_h);
