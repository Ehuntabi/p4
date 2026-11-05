# Parches al ESP-IDF

Aquí van los arreglos **de ESP-IDF** (no del proyecto) que hacen falta para
publicar, con el motivo y las pruebas. `release.sh` los aplica solo, de forma
idempotente, antes de compilar.

## `idf-5.5.5-spi-null-memcpy.patch`

**Qué es:** el arreglo oficial de Espressif
[`6c1f0e7b59`](https://github.com/espressif/esp-idf/commit/6c1f0e7b59e) —
*"fix(driver_spi): avoid NULL memcpy when private DMA buffer setup fails"*
(28-jul-2026, PR [#18898](https://github.com/espressif/esp-idf/pull/18898)).
Toca 3 líneas de `components/esp_driver_spi` (`spi_master.c`, `spi_slave.c`,
`spi_slave_hd.c`).

**Por qué hace falta aquí:** el IDF fijado es **v5.5.5** (etiquetado el
16-jul-2026), así que el arreglo es **posterior**. Entra en 5.5.6: **cuando se
suba el IDF, este parche sobra y hay que borrarlo** (`release.sh` avisa y no
hace nada si el IDF ya lo lleva).

**Qué pasaba sin él (reproducido en la P4 el 22-sep-2026):** cuando se agota la
RAM interna DMA-capaz —con tráfico Wi-Fi fuerte, p. ej. sacando las capturas por
TCP— el `spi_master` no puede reservar su buffer temporal y entra en su ruta de
error. Allí `uninstall_priv_desc()` hacía `memcpy(destino, NULL, n)` porque
`buffer_to_rcv` nunca llegó a asignarse: **`Load access fault` y reinicio** en
vez de devolver el error. La cadena exacta del log, justo antes de cada panic:

```
E dma_utils: esp_dma_capable_malloc(196): Not enough heap memory
E H_SDIO_DRV: task still writing Rx data to queue!
E spi_master: setup_dma_priv_buffer(1214): Failed to allocate priv RX buffer
Guru Meditation Error: Core 0 panic'ed (Load access fault)
   MEPC: memcpy  <- uninstall_priv_desc (spi_master.c:1182)
   <- setup_priv_desc (clean_up) <- spi_device_polling_start <- sdspi_host_start_command
```

Solo se disparaba al abrir las pantallas que **leen la SD** (6, 7, 8 y a veces
21: históricos de batería/frigo/solar y Victron Keys), porque son las que meten
tráfico de tarjeta en ese momento. En uso normal no se ha visto: hace falta que
el hueco DMA baje a cero, y eso pide mucha transferencia seguida.

**Comprobado en placa (22-sep-2026), con una prueba que fuerza el fallo a
propósito** (comerse la RAM DMA hasta dejar el bloque mayor por debajo de lo que
necesita una transferencia de SD, y pedir la pantalla 6):

| IDF | Resultado |
|---|---|
| v5.5.5 sin parche | `Load access fault` + reinicio (3 veces, sin forzar nada) |
| v5.5.5 + este parche | `Failed to allocate priv TX buffer` ×7, **0 panics, 0 reinicios**, la placa sigue y suelta la memoria |

Con el parche, el fallo se degrada como debe: el driver de la SD ve el error
(`sdspi_host.c` lo comprueba y lo devuelve), la lectura falla y esa pantalla se
queda sin datos. Nada de reiniciar.

## `idf-5.5.5-jpeg-buffer-bounds.patch`

**Qué es:** dos arreglos oficiales de Espressif que entran en **5.5.6**, traídos
a la 5.5.5:

- [`ceb7443c4da`](https://github.com/espressif/esp-idf/commit/ceb7443c4da)
  *"fix(jpeg): validate encoder buffer sizes before header/DMA access"*: acota la
  cabecera JPEG al tamaño del búfer de salida y **rechaza búferes de entrada
  demasiado pequeños antes de programar el DMA**, para no corromper el heap ni
  leer fuera de rango.
- [`4196c7ae5c8`](https://github.com/espressif/esp-idf/commit/4196c7ae5c8)
  *"fix(jpeg): release platform mutex on semaphore/pm-lock allocation failure"*:
  suelta el mutex de la plataforma si falla la reserva del semáforo o del PM lock
  (antes se quedaba cogido y el siguiente que lo pidiera se bloqueaba).

**Por qué hace falta aquí:** el IDF fijado es **v5.5.5**, anterior a los dos
arreglos. **Cuando se suba a 5.5.6 este parche sobra** (el aplicador lo detecta y
lo salta).

**Por qué NO se sube el IDF a 5.5.6 (probado el 05-nov-2026):** el 5.5.6
refactoriza el driver MIPI-CSI y `esp_video` activa la **conversión de formato en
el puente CSI**. El driver nuevo la rechaza si la revisión del chip es menor que
**v3.0**, y esta placa es **P4 rev v1.3**:

```
E CSI: esp_cam_new_csi_ctlr(207): failed to configure format conversion
E csi_video: csi_video_start(444): failed to start CSI ctlr
E camera: stream: STREAMON falla   -> /snapshot 503
```

Es un fallo conocido y abierto en Espressif
([issue #98](https://github.com/espressif/esp-video-components/issues/98)) y la
respuesta del mantenedor confirma que la conversión necesita ese driver. En una
placa v3.x funcionaría; en la v1.3 no hay forma. Probado también desactivando la
macro `ESP_VIDEO_CSI_DEVICE_CONV_FORMAT` en `esp_video`: sigue fallando, porque
el rechazo está en el driver del IDF.

**Comprobado en placa (05-nov-2026)** con la P4 rev v1.3: compilado con el
parche, `GET /snapshot` responde **200** con un JPEG de 960×528 (~88 KB) y el
mismo color medio que sin parche (227,38,25 frente a 229,37,25: lo rojo es lo que
tiene delante la cámara).
