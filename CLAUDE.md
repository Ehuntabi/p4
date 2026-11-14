# Joint SPL 145 Control (ex-VictronSolarDisplay) - Guition JC1060P470C_I

Repo: github.com/Ehuntabi/p4

> El último tag NO se escribe aquí: se consulta con `git describe --tags --match "v*.*"`
> (un solo punto: los tags son `vX.Y`, no `vX.Y.Z` — el mismo patrón que usa
> `CMakeLists.txt` para la versión embebida). Antes había un número fijo y llevaba
> doce versiones mintiendo (decía v1.6.6 estando en la v1.11.18, visto el
> 24-ago-2026). Un dato que hay que acordarse de actualizar a mano acaba
> engañando al que lo lea.

## Hardware
- ESP32-P4 (principal) + ESP32-C6 vía SDIO (Wi-Fi/BT con esp_hosted)
- Display DSI 1024x600 (panel JD9165BA)
- Touch GT911, RTC RX8025T (I²C 0x32, pila CR1220), microSD slot 0 IOMUX
- Codec audio ES8311 + amplificador NS4150 (GPIO11 PA_CTRL)
- Pines I2S: MCLK=GPIO13, BCLK=GPIO12, LRCK=GPIO10, DOUT=GPIO9
- Ventilador frigo PWM en GPIO5 (JP1 pin 15)
- Bus 1-Wire DS18B20 en GPIO4 (JP1 pin 13, pullup 4.7K a 3.3V)
- GPS u-blox NEO-M9N en UART2: RX=GPIO3, TX=GPIO2 (JP1), 38400 8N1. Ojo, 38400
  es el de fabrica de la serie M9; los M8 venian a 9600. Solo se escucha NMEA

## Stack
- ESP-IDF v5.5.5 (la que usan el CI y release.sh)
- LVGL para la UI
- Workspace: ~/joint/p4

## Forks de drivers vendorizados

Dos componentes traen parches locales sobre lo que sea que traiga el próximo
bump de IDF: `components/esp_lcd_jd9165/UPSTREAM.md` (fork del paquete
oficial del registry, v1.0.4, bypass del read-ID) y
`components/ov02c10/UPSTREAM.md` (port del PR
espressif/esp-video-components#46, sin mergear a fecha 09-sep-2026, con los
`tline_ns` que le faltan al PR). Cada uno documenta su origen exacto y el
procedimiento de re-sync al subir de versión — mirar ahí, no aquí, para no
mantener el mismo dato en dos sitios. El workaround BTA relacionado (mismo
panel JD9165, pero no es parte del componente) vive en
`main/esp_bsp.c:33-53` y se autoprotege con `_Static_assert` contra un
cambio de layout privado de IDF.

## El "destello claro" de la pantalla: era la telemetría (13-nov-2026)

**ESTA SECCIÓN CORRIGE A LA ANTERIOR.** Hasta el 12-nov aquí ponía que el
destello era eléctrico (el LED y su driver) y que no era cosa del firmware. Era
falso: se había medido con dos sondas que no valían (una pedía volcados de
1024x600 cuando el buffer de LVGL es de 50 líneas, o sea que no midió nunca; y
el contador de underruns del DSI no tenía control positivo).

Lo que sí se pudo medir, y quedó claro el 13-nov:

- El vídeo del destello (10x ralentizado, `/home/jc/Imágenes/video_flash_p4.mp4`)
  dice que **es luz, no datos**: durante el destello los píxeles más negros del
  panel siguen negros (10, 12, 20) y la imagen mantiene el contraste, así que no
  es el dato de reserva del DSI (0x3FFF, cian claro, que habría dejado esos
  píxeles en 123/255/255). Dura ~7 ms.
- Con la pantalla destellando, **ninguna sonda decía nada**: el duty del LEDC
  estaba donde el firmware lo puso, el pin no se clavaba, LVGL no mandaba ningún
  fotograma claro y el DSI no daba underruns. El firmware no tocaba la luz.
- Pasa **sin ningún periférico conectado**, y pasa igual a 12 V con DC/DC que
  con el USB del banco. La radio, la SD y las escrituras de flash llevan meses
  ahí sin producir destellos.
- Lo único nuevo en el camino periódico era la telemetría de `/api/perf`
  (commit `bb08e10`, 09-nov): cada 10 s llamaba a **`uxTaskGetSystemState()`**,
  que hace `vTaskSuspendAll()` y recorre las 40 tareas copiando un
  `TaskStatus_t` de cada una. Ese parón de milisegundos es un escalón de
  consumo, y en este módulo el 5 V lo hace un solo convertidor del que cuelgan a
  la vez el boost de la retroiluminación y la bomba (sin regular) que genera la
  polarización del panel: unos milivoltios se ven como un pulso de luz.
- **Demostrado con dos placas iguales**: con la llamada (v5.9) la pantalla
  destella; sin ella (v5.10) no destella ninguna de las dos.
  En `jantielens/esp32-macropad` (mismo chip, mismo tipo de panel) está
  documentada como la causa confirmada de sus destellos.

**Regla que queda:** no llamar a `uxTaskGetSystemState()` ni a nada que paralice
el planificador en un camino periódico de este aparato. El % de CPU del resumen
sale de los contadores de ralenti de cada núcleo
(`ulTaskGetIdleRunTimeCounterForCore`), que son dos lecturas de registro; la CPU
**por tarea** solo se pide a mano en `GET /api/perf`.

Para separar un reinicio del cable de uno de verdad: `reinicio` en `/api/perf` y
el `MOTIVO ULTIMO REINICIO` del log (grabar por USB deja **usb**; desenchufar la
pantalla, **poweron**).

## La sonda de aletas "a veces fallaba" (13-nov-2026)

Síntoma: el frigo **detectaba** la sonda de aletas (rol asignado, dirección
válida) pero cada tanto el ciclo se quedaba sin dato. Como es la que manda sobre
el ventilador, el ventilador se paraba en seco — y en el log **no quedaba ni
rastro**, porque solo se avisaba al cambiar el porcentaje.

Qué se cambió (v5.15, `components/frigo/frigo.c`):

- **Dos intentos** de lectura del scratchpad por ciclo: releerlo son 9 bytes, y
  el error suelto de CRC desaparece.
- Un fallo **no borra la sonda**: se aguanta el **último valor bueno** hasta
  **3 ciclos (~3 s)**. Pasado ese plazo sí se da por perdida y el ventilador se
  para, que sigue siendo lo seguro.
- Lecturas fuera de **-120..125 °C** se tratan como error (85,0 es el valor de
  power-on del DS18B20; -127 es "sin dato").
- **Aviso en el log con tope de uno por minuto**, con la dirección y el nombre
  del error.

Si vuelve a pasar, buscar en el log:

    frigo: sonda 0 (28:AA:BB:CC:DD:EE:FF:01) no lee: ESP_ERR_INVALID_CRC (seguidos 2, aguanto el ultimo valor)

Rachas cortas que se recuperan = ruido en el bus (cable, pull-up de 4,7 k,
longitud). `seguidos` que no para de subir = esa sonda o su cableado. La
dirección dice **cuál** de las tres es.

## Límite conocido: la pantalla parpadea mientras se graba flash (OTA)

No es un fallo del código y **no tiene arreglo por software en esta placa**: el
panel MIPI-DSI lee su framebuffer de la PSRAM y, mientras la OTA borra/escribe la
flash, esas lecturas no llegan a tiempo (underrun del DSI: el panel se va a
negro/azul y vuelve). Descartado con medidas y con log, no por intuición:
`CONFIG_SPI_FLASH_AUTO_SUSPEND` (el chip es un **BOYA** y su driver dice
"flash-suspend is not supported"), el QoS del AXI-ICM para el DMA2D (no cambia
nada y el burst del puente DSI a 64 B lo **empeora**), el DPI a 42 MHz (quita los
underruns pero el panel muestra **los colores cambiados**), el bounce buffer (no
existe en el DPI del P4) y el framebuffer en RAM interna (1,2 MB contra los
~768 KB del P4; además el heap interno tiene ~2 MB de mínimo histórico con la
cámara, el Wi-Fi y los buffers de audio dentro).

Por eso la OTA **enseña el aviso con la pantalla encendida y después la apaga**
mientras graba (`main/portal/ota_update.c`). Si algún día se "arregla" el
parpadeo, hay que poder borrar ese apagado: por eso el brillo anterior se guarda
y se restaura en TODAS las salidas. Detalle completo y referencias en `NOTAS.md`
(v4.35). Ojo con un detalle del reinicio de la OTA: es un reset **por software**,
el panel no se reinicializa y su RAM conserva el último fotograma, así que
`main.c` fuerza un repintado completo al arrancar.

## Avisos de arranque que son NORMALES (no perseguir)

Los tres salen en cada arranque y ninguno es un problema; están aquí para no
volver a investigarlos:

- `spi_flash: Detected boya flash chip but using generic driver. For optimal
  functionality, enable SPI_FLASH_SUPPORT_BOYA_CHIP` — el driver BOYA de IDF solo
  cambia la detección y cómo se pone el bit QE al arrancar (BOYA hereda la de GD);
  borrados, escrituras y tiempos son el MISMO código genérico, y no añade
  flash-suspend (el propio driver dice que no lo soporta). `flash io: qio` en el
  log demuestra que el camino genérico acierta. Activarlo no cambia la OTA.
- `lcd_panel: esp_lcd_panel_swap_xy(50): swap_xy is not supported by this panel` —
  el panel JD9165 no soporta intercambiar ejes; se pide en el BSP y se ignora.
- `gps: NO llega NADA por el UART2` — el módulo GPS no está puesto en el banco.

## Comandos habituales
- Entorno IDF (necesario antes de compilar/flashear): `. ~/.espressif/esp-idf-5.5/export.sh`
- Compilar: `idf.py build`
- Flashear: **`scripts/grabar.sh [puerto]`** desde la raiz del repo (el puerto por
  defecto es `/dev/ttyACM0`; a veces es ttyACM1). Ese script NO graba si lo que
  se va a grabar no esta publicado: exige arbol limpio, tag de la version y
  Release en GitHub (16-oct-2026, "esto no puede pasar": la P4 quedo con la v4.38
  grabada y GitHub enseñando la v4.37). Para una prueba en el banco con algo sin
  publicar: `GRABAR_SIN_PUBLICAR=1 scripts/grabar.sh`.
  A mano, sin las comprobaciones, sigue siendo `idf.py -p /dev/ttyACM0 flash`,
  pero es el camino por el que se grabo la v4.38 con la Release en la v4.37 (y el
  que recompila antes de grabar): usa `scripts/grabar.sh`. Ese script comprueba
  ADEMAS que el asset de la Release es el de ESTA version (nombre exacto y
  sha256), que el chip del puerto es un ESP32-P4 (no una cabina) y, si el .bin de
  aqui no es byte a byte el publicado, graba EL PUBLICADO.
  - **TRAMPA (6-oct-2026, costo media hora):** despues de una OTA la placa queda
    arrancando de la OTRA particion (`ota_1`, 0x410000), asi que un
    `write_flash ... 0x10000` se escribe en `ota_0` **y no arranca**: la placa
    sigue con el firmware viejo y parece que el cambio "no se ve". Para dejarlo
    claro por USB:
    `python -m esptool --chip esp32p4 -p /dev/ttyACM0 write_flash 0xd000 build/ota_data_initial.bin 0x10000 build/joint_spl_145_control.bin 0x410000 build/joint_spl_145_control.bin`
    (ota_data_initial reinicia el registro de arranque; NVS y SPIFFS no se tocan).
    La forma de comprobarlo es el log de arranque: `### FIRMWARE: vX compilado <fecha> ###`.
- Monitor: `idf.py -p /dev/ttyACM0 monitor`
- OTA habitual: subir directamente por Wi-Fi desde este portatil a
  `http://192.168.4.1:8081/ota` (pide Basic Auth strict: usuario/clave de
  Ajustes -> Wi-Fi). Puerto 8081 desde el 09-sep-2026: OTA vive en la
  instancia httpd "pesada" (junto con los .tar y la galeria de vigilancia),
  separada de la principal (puerto 80) para que una subida larga no deje
  mudo el resto del portal (`/api/state`, la web). El USB queda solo para
  cuando hace falta diagnosticar un arranque (log de boot, crash, NVS) —
  sacar la placa de donde esta instalada es un engorro.

## CI
- `.github/workflows/build.yml` (2026-08-14): compila con `idf.py build`
  en cada push/PR via `espressif/esp-idf-ci-action@v1` (docker
  `espressif/idf:v5.5.5`, target `esp32p4`). Solo pilla errores de
  COMPILACION, no sustituye probar en la placa.
- Job `mini_proto_sync` (07-sep-2026): compara byte a byte
  `main/net/mini_proto.h` contra la copia de `Ehuntabi/cabina5` (protocolo UDP
  compartido) y falla si difieren. La 3,5" (`35cabina`) se retiró del CI el
  09-nov-2026: el dueño no la usará nunca más, así que el único satélite es la
  5" y no hay que portar los cambios a ningún otro sitio.

## Versionado / releases
- La version que se ve en Ajustes -> Acerca de sale SOLA de `git describe`
  (esp_app_get_description()->version). NO se edita ningun numero en el codigo.
- **Regla (13-sep-2026, decisión del usuario): la versión NUNCA lleva sufijos.**
  Ni `-dirty` ni `-N-g<hash>`: un binario se llama `v2.6` y punto. Lo que hay por
  encima del último tag **es** la versión siguiente, **+0.1** (`v2.5` + trabajo
  sin etiquetar -> `v2.6`). Ese cálculo vive en `CMakeLists.txt` (versión
  embebida) y en `scripts/bin_con_version.sh` (nombre del fichero).
- **Todo build deja una copia con la versión en el nombre**:
  `build/joint_spl_145_control_v2.6.bin`, junto al binario de siempre (el
  original `joint_spl_145_control.bin` no se toca: es el que usan `idf.py flash`
  y `flash_args`). Lo hace el target `bin_con_version` de `CMakeLists.txt` (tiene
  que ir DESPUÉS de que esptool genere el `.bin`: colgado del `POST_BUILD` del
  `.elf` copiaba el binario de la compilación anterior), así que no hay que
  acordarse de nada; el build imprime las rutas con `[bin]`.
- **La copia va además a `~/joint-releases`**, con el mismo nombre que publica
  `release.sh` para OTA (`joint-spl-145-control-vX.Y-app.bin`): es la carpeta
  donde se sabe que está lo que se lleva a la autocaravana. Ahí **se deja solo la
  última versión** del P4 —las viejas se retiran, igual que hace `release.sh`
  (siguen en la Release de GitHub y en su tag)— sin tocar nada de otros
  proyectos (`35cabina`) ni la app Flutter.
- Para publicar: `./release.sh [X.Y]` (crea el tag, build limpio anti-gotcha,
  imagen fusionada, y publica la Release). **Sin X.Y coge el último tag +0.1.**
- Gotcha ESP-IDF: la version/fecha se cachea en el configure de CMake; si el About
  muestra datos viejos, `idf.py reconfigure` + borrar el .obj de esp_app_desc lo
  refresca (release.sh ya lo hace). Desde el 13-sep-2026
  `scripts/bin_con_version.sh` avisa en el build cuando el binario lleva
  embebida una versión distinta de la del árbol.

## ⚠️ Trampa recurrente: `dependencies.lock` y la ruta de esp_hosted

**Cada `idf.py build` que re-resuelva dependencias reescribe `dependencies.lock`
con la RUTA ABSOLUTA de la máquina** para el componente local
`components/espressif__esp_hosted`:

```
path: /home/<usuario>/joint/p4/components/espressif__esp_hosted
```

En GitHub Actions esa ruta no existe y **el CI falla** con
`ERROR: The "path" field in the manifest file ... does not point to a directory`.

Ha pasado ya **tres veces** (commits `e3f710d`, `314a41b`, `edd2a8a`), una de
ellas desde otro equipo (`/home/db3/...`).

**Protecciones (auditadas el 15-sep-2026, antes solo existian de forma local):**

1. **Hook versionado en `.githooks/pre-commit`**: aborta el commit si el
   `dependencies.lock` que va a entrar lleva una ruta absoluta. En un clon
   nuevo hay que activarlo UNA vez: `git config core.hooksPath .githooks`
   (este repo ya lo tiene puesto).
2. **Bit `skip-worktree`** (local, no versionable): **a 17-oct-2026 NO esta
   puesto** en este equipo — `git ls-files -v dependencies.lock` da `H` (normal),
   no `S`, y por eso `git status` lo enseña modificado con la ruta absoluta
   dentro tras un build. Se pone con `git update-index --skip-worktree
   dependencies.lock` (y entonces sí sale `S`); hace que git ignore el churn que
   los builds de este equipo le meten al fichero. Quitarlo:
   `git update-index --no-skip-worktree dependencies.lock`.

El chequeo rapido sigue siendo util: `grep -n "path: /" dependencies.lock` —
pero ojo, en este equipo devuelve resultado SIN que haya problema (el build
reescribe el fichero con la ruta absoluta). Con el skip-worktree puesto git no
lo ve; sin el, sale como modificado, pero el hook no deja commitear la ruta
absoluta. Lo que decide de verdad es el hook, que mira lo que SE VA A COMMITEAR.

## Convenciones del proyecto
- Estética card-based aplicada en Settings (cada página con su color de borde)
- Textos en español, sin emojis (excepto símbolos LV_SYMBOL_*)
- Persistencia con NVS por componente (namespace propio)
- Logs sin acentos para evitar problemas de codificación

## Estructura clave
- main/main.c: app_main() son 9 llamadas a init_*() en fases (una por
  seccion del arranque: display/UI, SD+RTC+frigo, red, telemetria,
  audio+BLE, timers periodicos, camara...), todas en el mismo fichero
  (`init_boot_diagnostics`, `init_nvs_wifi_pass_watchdog`, `init_display_ui`,
  `init_sd_rtc_frigo`, `init_network`, `init_telemetry`, `init_audio_ble`,
  `init_periodic_timers`, `init_camera_and_solar_feed`; ver main/main.c:982-991)
- main/ui.c: tabview, barra inferior, screensaver, hora
- main/ui/ agrupado por tema (2026-08-14, antes plano):
  - settings/: settings_panel.c (orquestador: paginas SD, Autocaravana,
    screensaver/modo nocturno) + paginas concretas (settings_wifi.c,
    settings_display.c, settings_victron_keys.c, settings_about.c,
    settings_sound.c, trip_manager.c, settings_dialogs.c)
  - views/: frigo_panel.c y el resto de pantallas (view_overview.c,
    view_battery_monitor.c, view_simple*.c, view_solar_charger.c...)
  - devices/: device_tracker.c, ble_ingest.c
  - history/: battery_history_screen.c, frigo_history_screen.c, chart_common.c
  - vigilancia/: ausente_mode.c, gallery.c, capture_carousel.c
  - widgets/: ui_card.c, ui_format.c, ui_state.h, lv_font_thermometer.c
- main/portal/ (2026-08-14, antes plano en main/):
  - config_server.c: portal HTTP — handlers core (root, keys, static,
    save, api_state, dashboard, settime, capturas) + arranque/registro de
    URIs del httpd y del DNS del captive portal. DOS instancias httpd desde
    el 09-sep-2026: la principal (puerto 80, `s_httpd`) y otra "pesada"
    (puerto 8081, `s_httpd_heavy`, ver PORTAL_HEAVY_BASE en
    config_server_internal.h) solo para OTA, los .tar y la galeria de
    vigilancia — esp_http_server es de una sola tarea, y una de esas
    peticiones dejaba mudo TODO lo demas (incluido /api/state) mientras
    duraba. cfg_http_stop() para las dos.
  - config_server_ap.c: ciclo de vida del AP Wi-Fi — radio (wifi_ap_init),
    timers de auto-off del HTTP, cola de trabajos (start/stop/apply);
    expone cfg_http_stop()/cfg_dns_stop() (definidas en config_server.c)
    para pararlo sin tocar sus estaticos
  - config_server_vigilancia.c (galeria /snapshot y /vigilancia),
    config_server_auth.c (Basic Auth + servido de SPIFFS), charts_svg.c,
    data_export_tar.c, ota_update.c (actualizacion de firmware por Wi-Fi)
- main/data/ (2026-08-14, antes plano en main/): capa de estado/telemetria
  compartida por el portal y las pantallas de ui/ — dashboard_state.c
  (/api/state), energy_today.c, solar_daily.c, trip_computer.c
- components/audio_es8311/: codec con jingles BOOT_OK/CRITICAL/WARNING/CONFIRM
- components/alerts/: thresholds NVS (freezer/SoC)
- components/config_storage/: persistencia general (Wi-Fi, screensaver, etc.)

## Pendientes activos
1. **La 3,5" está RETIRADA** (09-nov-2026, decisión del dueño: "olvídala para
   siempre"). Su repo `~/joint/35cabina` queda congelado como histórico: no se
   sincroniza con la P4, no está en el CI y no hay que portarle nada. El único
   satélite en uso es la 5" (`~/joint/cabina5`).

   Al día 24-ago-2026: los apuntes sueltos ya se envían y se suman en el
   `resumen.txt`, la energía del viaje la abre y la cierra el propio viaje, y
   la P4 guarda además los kilómetros y el recorrido (`ruta.csv`) por GPS.

   **Sobre el simulador**, que se usa para las capturas: además de inventar lo
   que se ve en pantalla, **escribe un día entero inventado** en
   `/sdcard/frigo/<fecha>.csv` y `/sdcard/bateria/<fecha>.csv` (el solar NO lo
   toca). Desde v1.6.6 deja rastro y **se deshace solo**: el registro real se
   aparta como `<csv>.real`, y si no había ninguno se deja una marca vacía
   `<csv>.sim`. `sim_overview_restaurar_reales()` corre en cada arranque de
   producción y devuelve o borra según la marca. Ya no hay que acordarse de
   nada, pero **mientras el simulador esté puesto la P4 enseña datos FALSOS**:
   - `SIM_OVERVIEW_ENABLE` a `1` en `main/sim_overview.h`
   - descomentar `sim_overview_start();` en `main/main.c`

2. **PWM del ventilador a 25 kHz — RESUELTO por hardware el 12-nov-2026.** La PCB
   nueva quita el PC817 y ataca el IRLR7843 **directo** desde GPIO5 (R3 100 Ω serie,
   R4 10k de pull-down, D2 de rueda libre, C2 100 µF). Con eso el cuello de botella
   del opto desaparece y la frecuencia se sube a **25000 Hz** en `frigo.h`. Queda
   pendiente comprobarlo en el frigo real (arranque desde parado y temperatura del
   MOSFET); si arranca peor, volver a 18000. El texto de abajo se deja como
   histórico del motivo por el que estuvo en 18 kHz tanto tiempo.

   (Histórico) **PWM del ventilador a 25 kHz — NO es solo cambiar la línea** (27-jul-2026).
   Ahora está a 18 kHz (`FRIGO_FAN_FREQ_HZ` en `components/frigo/frigo.h`): inaudible
   para adultos, pero **NO para oídos jóvenes** (su límite ronda los 19-20 kHz). Por
   encima de 20 kHz no lo oye nadie.
   **⚠️ EL CUELLO DE BOTELLA ES EL PC817, NO EL MOSFET.** El ataque actual es
   P4 → **optoacoplador PC817** → **IRLR7843** (R serie en puerta + R pull-down + diodo
   1N4148 de rueda libre). El IRLR7843 conmuta en ns y LEDC llega a ~39 kHz con los
   10 bits actuales (sin perder finura), pero el **PC817 tarda microsegundos**, y el
   apagado bastante más que el encendido. A 18 kHz el ciclo son 55 µs y a 25 kHz solo
   40 µs, así que las transiciones del opto se comen una fracción cada vez mayor.
   **Se degrada sobre todo a duty bajo**: al 30 % el pulso dura ~12 µs y el opto casi no
   llega a conmutar → control no lineal por abajo.
   Por tanto, para subir a 25 kHz hay que **tocar el hardware primero**, eligiendo una:
   (a) sustituir el PC817 por un opto rápido (6N137, o driver aislado tipo TLP250);
   (b) quitar el opto si no hace falta aislamiento — el IRLR7843 es de puerta lógica y
       el P4 puede atacarlo directo con la R serie (la opción más simple);
   (c) quedarse en 18 kHz.
   Al cambiarlo, comprobar en el frigo real: (1) que **sigue arrancando desde parado**
   (desde v5.14 no hay kickstart: la rampa arranca en el suelo de duty, 30 %, y sube
   10 %/s desde v5.18 —1 % cada 100 ms, 10 s de 0 a 100—; el mismo camino al parar) y (2) que el MOSFET no calienta.
   Si arranca peor, volver a 18 kHz.
   (Histórico: antes del IRLR7843 había un módulo D4184, limitado a ≤20 kHz. Ese límite
   ya no aplica, pero apareció el del opto.)
   **Aplazado a propósito (15-ago-2026):** decisión del usuario, se retoma cuando
   llegue la placa que sustituye a la actual (para hacer el cambio de opto de una vez).
   Mientras tanto 18 kHz va bien, no es urgente.
