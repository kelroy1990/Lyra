# Lyra - Hi-Res USB DAC & Audio Player

Reproductor de audio portatil de alta resolucion basado en ESP32-P4 con salida balanceada 4.4mm, pantalla tactil y conectividad inalambrica.

---

## Indice

1. [Hardware](#1-hardware)
2. [Arquitectura de Software](#2-arquitectura-de-software)
3. [Estructura del Proyecto](#3-estructura-del-proyecto)
4. [Implementacion USB](#4-implementacion-usb)
5. [Pipeline de Audio](#5-pipeline-de-audio)
6. [Sistema de Tareas (FreeRTOS)](#6-sistema-de-tareas-freertos)
7. [Presupuesto de Memoria](#7-presupuesto-de-memoria)
8. [Fases de Desarrollo](#8-fases-de-desarrollo)
9. [Build y Flash](#9-build-y-flash)

---

## 1. Hardware

### 1.1 MCU - ESP32-P4NRW32

| Parametro           | Valor                                    |
|---------------------|------------------------------------------|
| CPU                 | Dual RISC-V @ 400 MHz                   |
| SRAM interna        | 768 KB                                   |
| PSRAM (in-package)  | 32 MB                                    |
| Cristal principal   | 40 MHz                                   |
| Cristal RTC         | 32 kHz (dominio VBAT para deep sleep)    |
| USB                 | OTG 2.0 High-Speed (480 Mbps), PHY UTMI |
| HW Shutdown         | Pin dedicado para apagado hardware       |

### 1.2 Audio

| Componente        | Especificacion                                        |
|-------------------|-------------------------------------------------------|
| DAC               | ES9039Q2M (SPI control, I2S data)                     |
| Formato maximo    | PCM 384 kHz / 32-bit / Stereo                         |
| DSD               | DSD over PCM (DoP) soportado                          |
| MCLK              | 6.144 MHz (96k), 12.288 MHz (192k), 24.576 MHz (384k)|
| Salida            | Jack 4.4mm balanceado con deteccion GPIO              |
| Alimentacion      | LDO dedicado para +-10V limpio (EN por GPIO)          |

### 1.3 Pantalla

| Parametro    | Valor                          |
|--------------|--------------------------------|
| Resolucion   | 720 x 1280                     |
| Interfaz     | MIPI DSI (PPI nativo del P4)   |
| Touch        | Panel tactil I2C               |

### 1.4 Almacenamiento

| Medio    | Interfaz | Notas                      |
|----------|----------|----------------------------|
| microSD  | SDIO     | FAT/exFAT, audio local     |

### 1.5 Conectividad Inalambrica

| Componente | Interfaz | Capacidades             |
|------------|----------|-------------------------|
| ESP32-C5   | SDIO     | BT5 + WiFi6             |

Uso: streaming BT a auriculares, app companion, OTA.

### 1.6 Gestion de Energia

| Componente     | Interfaz | Funcion                              |
|----------------|----------|--------------------------------------|
| MAX77972EWX+   | I2C      | Cargador Li-ion + fuel gauge (combo) |
| Bateria        | -        | Single-cell Li-ion                   |
| RTC + VBAT     | -        | 3V desde bateria, wake desde sleep   |
| Pin INT        | GPIO     | Interrupcion del MAX77972            |

### 1.7 Sensores

| Sensor  | Interfaz | Funcion                               |
|---------|----------|---------------------------------------|
| BMA400  | I2C      | Acelerometro (orientacion, gestos)    |
| Pin INT | GPIO     | Interrupcion del BMA400               |

### 1.8 Controles Fisicos

5 botones GPIO: Play/Pause, Vol+, Vol-, Previous, Next.

### 1.9 Mapa de Perifericos

| Periferico | Bus   | Dispositivo    | Funcion                  |
|-----------|-------|----------------|--------------------------|
| I2S        | -     | ES9039Q2M      | Datos de audio           |
| SPI        | SPI   | ES9039Q2M      | Control de registros DAC |
| I2C        | I2C   | Touch panel    | Pantalla tactil          |
| I2C        | I2C   | MAX77972       | Cargador + fuel gauge    |
| I2C        | I2C   | BMA400         | Acelerometro             |
| SDIO       | SDIO  | microSD        | Almacenamiento local     |
| SDIO       | SDIO  | ESP32-C5       | Companion inalambrico    |
| MIPI DSI   | PPI   | Display        | 720x1280                 |
| USB OTG    | USB   | Host PC        | UAC2 + CDC + MSC         |
| GPIO       | -     | DAC EN         | Habilitar DAC            |
| GPIO       | -     | Audio LDO EN   | Habilitar +-10V audio    |
| GPIO       | -     | Jack 4.4mm     | Deteccion conexion       |
| GPIO       | -     | MAX77972 INT   | Interrupcion cargador    |
| GPIO       | -     | BMA400 INT     | Interrupcion acelerometro|
| GPIO       | -     | HW Shutdown    | Apagado hardware ESP32   |
| GPIO       | -     | Botones (5)    | Controles de usuario     |

---

## 2. Arquitectura de Software

### 2.1 Diagrama de Bloques

```
+------------------+     +-------------------+
|   USB Host (PC)  |     |   microSD (SDIO)  |
+--------+---------+     +---------+---------+
         |                         |
    USB OTG 2.0 HS           SDIO/SDMMC
         |                         |
+--------v---------+     +---------v---------+
|   usb_device     |     |     storage       |
|  UAC2+CDC+MSC    |     |   FAT/exFAT FS    |
+--------+---------+     +---------+---------+
         |                         |
         +-------+     +----------+
                 |     |
          +------v-----v------+
          |  audio_pipeline   |
          |  Source Manager   |
          +--------+----------+
                   |
          +--------v----------+
          |   DSP / EQ Chain  |
          +--------+----------+
                   |
          +--------v----------+
          |   I2S DMA Output  |
          +--------+----------+
                   |
          +--------v----------+
          |    ES9039Q2M DAC  |
          +--------+----------+
                   |
          +--------v----------+
          | 4.4mm Balanced Out|
          +-------------------+

+------------------+     +-------------------+
|     display      |     |      power        |
|  MIPI DSI + LVGL |     | MAX77972 + Battery|
+--------+---------+     +---------+---------+
         |                         |
+--------v---------+     +---------v---------+
|       ui         |     |     sensors       |
| Screens + Menus  |     |      BMA400       |
+------------------+     +-------------------+

+------------------+     +-------------------+
|      input       |     |    wireless       |
|  5 GPIO Buttons  |     | ESP32-C5 via SDIO |
+------------------+     +-------------------+
```

### 2.2 Dispositivo USB Compuesto

```
Host PC detecta:
  IAD #1 : UAC2 Speaker    (ITF 0 Audio Control, ITF 1 Audio Streaming)
  IAD #2 : CDC ACM Serial  (ITF 2 CDC Control,   ITF 3 CDC Data)
  IAD #3 : MSC             (ITF 4 Mass Storage)   [planificado - F6]
```

### 2.3 Asignacion de Endpoints

| Direccion EP | Direccion | Tipo         | Uso                      |
|-------------|-----------|--------------|--------------------------|
| 0x01        | OUT       | Isochronous  | Audio data (host -> DAC) |
| 0x81        | IN        | Isochronous  | Audio feedback           |
| 0x82        | IN        | Interrupt    | CDC notification         |
| 0x03        | OUT       | Bulk         | CDC data OUT             |
| 0x83        | IN        | Bulk         | CDC data IN              |

### 2.4 Flujo de Audio USB (UAC2 Asincrono)

1. Host establece sample rate via Clock Set Request
2. Host abre streaming interface (alt setting 1)
3. Host envia audio PCM por EP OUT isocrono
4. `audio_task` lee del FIFO TinyUSB cada 1 ms via `tud_audio_read()`
5. Feedback EP reporta nivel del FIFO (`AUDIO_FEEDBACK_METHOD_FIFO_COUNT`)
6. Audio se envia a I2S DMA -> ES9039Q2M

### 2.5 Flujo CDC (printf sobre USB)

```
printf() -> stdout -> freopen("/dev/usbcdc") -> cdc_vfs_write() -> tud_cdc_write() -> USB CDC IN
```

- Conversion LF -> CRLF para terminales
- Thread-safe con `_lock_acquire_recursive`
- Non-blocking: descarta datos si CDC no conectado
- Registrado despues de `tusb_init()`, antes de crear tareas

---

## 3. Estructura del Proyecto

```
lyra/
├── CMakeLists.txt                          # Raiz del proyecto ESP-IDF
├── README.md                               # Este archivo
├── main/
│   ├── CMakeLists.txt                      # Build config del componente main
│   ├── idf_component.yml                   # Dependencia: espressif/tinyusb
│   ├── app_main.c                          # Entry point, init de subsistemas
│   ├── tusb_config.h                       # Configuracion TinyUSB
│   ├── usb_descriptors.h                   # IDs de entidad, enum de interfaces
│   ├── usb_descriptors.c                   # Callbacks de descriptores USB
│   ├── usb_cdc_vfs.h                       # Header del driver VFS CDC
│   └── usb_cdc_vfs.c                       # Driver VFS para printf -> USB CDC
└── components/
    ├── usb_device/                         # TinyUSB: UAC2 + CDC + MSC
    │   ├── usb_device.c
    │   └── include/usb_device.h
    ├── audio_pipeline/                     # Source mgr, DSP, I2S output
    │   ├── audio_pipeline.c
    │   └── include/audio_pipeline.h
    ├── audio_codecs/                       # Decoders: FLAC, MP3, WAV, AAC
    │   ├── audio_codecs.c
    │   └── include/audio_codecs.h
    ├── display/                            # MIPI DSI driver + LVGL init
    │   ├── display.c
    │   └── include/display.h
    ├── ui/                                 # Pantallas, menus, state machine
    │   ├── ui.c
    │   └── include/ui.h
    ├── input/                              # GPIO buttons + power button
    │   ├── input.c
    │   └── include/input.h
    ├── storage/                            # microSD (SDIO) + filesystem
    │   ├── storage.c
    │   └── include/storage.h
    ├── power/                              # MAX77972 I2C, bateria, carga
    │   ├── power.c
    │   └── include/power.h
    ├── sensors/                            # BMA400 acelerometro
    │   ├── sensors.c
    │   └── include/sensors.h
    └── wireless/                           # SDIO <-> ESP32-C5, BT/WiFi
        ├── wireless.c
        └── include/wireless.h
```

### 3.1 Integracion TinyUSB (sin esp_tinyusb)

Se usa `espressif/tinyusb` directamente (el wrapper `esp_tinyusb` no soporta audio class).

**Patron de integracion:**

1. Declarar dependencia en `main/idf_component.yml`:
   ```yaml
   dependencies:
     espressif/tinyusb: ">=0.19.0~2"
   ```

2. Inyectar `tusb_config.h` en el build de TinyUSB:
   ```cmake
   idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
   target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
   ```

3. Forzar enlace de callbacks de descriptores:
   ```cmake
   target_link_libraries(${COMPONENT_LIB} INTERFACE
       "-u tud_descriptor_device_cb"
       "-u tud_descriptor_configuration_cb"
       "-u tud_descriptor_string_cb")
   ```

### 3.2 Correccion EP Size para High-Speed

TinyUSB asume frames de 1 ms (Full-Speed), pero en HS con `bInterval=1` los microframes son de 125 us. Macro corregida:

```c
#define TUD_AUDIO_EP_SIZE_HS(_maxFreq, _nBytesPerSample, _nChannels) \
    ((_maxFreq / 8000 + 1) * _nBytesPerSample * _nChannels)
```

A 384 kHz/32-bit/stereo: `(384000/8000 + 1) * 4 * 2 = 392 bytes` por microframe (dentro del limite de 1024).

---

## 4. Implementacion USB

### 4.1 Configuracion de Audio (tusb_config.h)

| Parametro                        | Valor    |
|----------------------------------|----------|
| Sample rate maximo               | 384 kHz  |
| Canales RX                       | 2 (stereo)|
| Bytes por muestra                | 4 (32-bit)|
| Resolucion                       | 32 bits  |
| EP OUT SW buffer                 | 32x EP size max |
| Feedback EP                      | Habilitado (asincrono) |
| Modo USB                         | High-Speed (480 Mbps)  |

### 4.2 Formatos Soportados

@TODO Agregar los de 24 y 16.
| Sample Rate | Bit Depth | Canales | MCLK        | Ancho Banda USB |
|-------------|-----------|---------|-------------|-----------------|
| 44,100 Hz   | 32-bit    | Stereo  | 6.144 MHz   | 2.82 Mbps       |
| 48,000 Hz   | 32-bit    | Stereo  | 6.144 MHz   | 3.07 Mbps       |
| 88,200 Hz   | 32-bit    | Stereo  | 12.288 MHz  | 5.64 Mbps       |
| 96,000 Hz   | 32-bit    | Stereo  | 12.288 MHz  | 6.14 Mbps       |
| 176,400 Hz  | 32-bit    | Stereo  | 24.576 MHz  | 11.29 Mbps      |
| 192,000 Hz  | 32-bit    | Stereo  | 24.576 MHz  | 12.29 Mbps      |
| 352,800 Hz  | 32-bit    | Stereo  | 24.576 MHz  | 22.58 Mbps      |
| 384,000 Hz  | 32-bit    | Stereo  | 24.576 MHz  | 24.58 Mbps      |

### 4.3 Entidades UAC2

| Entity ID | Tipo              | Descripcion            |
|-----------|-------------------|------------------------|
| 0x04      | Clock Source      | Reloj interno programable |
| 0x01      | Input Terminal    | USB Streaming input    |
| 0x02      | Feature Unit      | Mute + Volume (per-ch) |
| 0x03      | Output Terminal   | Desktop Speaker        |

### 4.4 Interfaces USB

| Interface | Uso                | String Descriptor |
|-----------|--------------------|-------------------|
| ITF 0     | Audio Control      | "UAC2 Speaker"    |
| ITF 1     | Audio Streaming    | -                 |
| ITF 2     | CDC ACM Control    | "CDC Serial"      |
| ITF 3     | CDC Data           | -                 |

---

## 5. Pipeline de Audio

```
USB UAC2 FIFO  ──┐
                  ├──> Source Manager ──> Ring Buffer (PSRAM)
microSD Decoder ──┘         |
                            v
                    EQ / DSP Processing
                            |
                            v
                     I2S DMA Buffer
                            |
                            v
                    ES9039Q2M DAC (I2S)
                            |
                            v
                   4.4mm Balanced Output
```

### 5.1 Control del DAC

| Senal         | Tipo | Funcion                              |
|---------------|------|--------------------------------------|
| DAC EN        | GPIO | Habilitar/deshabilitar el ES9039Q2M  |
| Audio LDO EN  | GPIO | Habilitar alimentacion +-10V limpia  |
| Jack Detect   | GPIO | Detectar conexion jack 4.4mm         |
| SPI           | Bus  | Configurar registros del DAC         |
| I2S           | Bus  | Transmitir datos PCM al DAC          |

### 5.2 Secuencia de Encendido del Audio

1. Activar Audio LDO EN (+-10V estabiliza)
2. Activar DAC EN
3. Configurar ES9039Q2M via SPI (formato, filtro, volumen)
4. Iniciar I2S con MCLK correspondiente al sample rate
5. Comenzar transferencia de audio

---

## 6. Sistema de Tareas (FreeRTOS)

| Tarea             | Prioridad | Core | Funcion                           |
|-------------------|-----------|------|-----------------------------------|
| tusb_device_task  | 5         | 1    | Manejo de eventos USB TinyUSB     |
| audio_task        | 4         | 1    | Pipeline de audio (FIFO -> I2S)   |
| ui_task           | 3         | 0    | LVGL display + touch              |
| system_task       | 2         | 0    | Energia, botones, sensores        |

**Asignacion por core:**
- **Core 0:** UI + sistema (no tiempo real)
- **Core 1:** USB + audio (tiempo real, baja latencia)

---

## 7. Presupuesto de Memoria

### 7.1 SRAM Interna (768 KB)

| Uso                        | Estimacion |
|----------------------------|------------|
| FreeRTOS kernel + stacks   | ~40 KB     |
| USB EP buffers + FIFO      | ~16 KB     |
| I2S DMA buffers            | ~8 KB      |
| TinyUSB internal           | ~12 KB     |
| Variables globales / BSS   | ~20 KB     |
| **Disponible**             | **~672 KB**|

### 7.2 PSRAM (32 MB)

| Uso                           | Estimacion |
|-------------------------------|------------|
| Framebuffer display (720x1280x2) | ~1.8 MB |
| LVGL working buffers          | ~1.8 MB    |
| Audio ring buffer (USB)       | ~512 KB    |
| Audio ring buffer (decode)    | ~512 KB    |
| Decoded audio cache           | ~2 MB      |
| File I/O buffers              | ~256 KB    |
| LVGL objects + themes         | ~1 MB      |
| **Disponible**                | **~24 MB** |

### 7.3 Flash

| Uso                   | Estimacion |
|-----------------------|------------|
| Firmware              | ~1-2 MB    |
| LVGL assets + fonts   | ~2-4 MB    |
| NVS (configuracion)   | ~64 KB     |

---

## 8. Fases de Desarrollo

| Fase | Nombre                    | Componentes              | Estado       |
|------|---------------------------|--------------------------|--------------|
| F0   | Estructura del proyecto   | Todos (placeholder)      | ✅ Completado |
| F0.5 | USB Audio (UAC2+CDC)      | usb_device, TinyUSB      | ✅ Completado |
| F1   | I2S output a ES9039Q2M    | audio_pipeline           | 🟡 Temporal (ES8311) |
| F2   | Display & UI base         | display, ui              | ⏸️ Pendiente  |
| F3   | EQ / DSP                  | audio_pipeline           | 🔄 En planificación |
| F4   | Gestion de energia        | power, sensors           | ⏸️ Pendiente  |
| F5   | Controles fisicos         | input                    | ⏸️ Pendiente  |
| F6   | Reproduccion microSD      | storage, audio_codecs    | ⏸️ Pendiente  |
| F7   | Wireless ESP32-C5         | wireless                 | ⏸️ Pendiente  |
| F8   | UI avanzada               | ui                       | ⏸️ Pendiente  |
| F9   | Polish y features avanzados| Todos                   | ⏸️ Pendiente  |

### F0 - Estructura del Proyecto (completado)
- Renombrar `hello_world_main.c` -> `app_main.c`
- `project(lyra)` en CMakeLists.txt raiz
- Crear 10 directorios de componentes con placeholders

### F1 - I2S Output a ES9039Q2M
- Configurar I2S para 32-bit stereo con generacion MCLK
- Control GPIO: DAC EN, Audio LDO EN, Jack 4.4mm detect
- Interfaz SPI para registros del ES9039Q2M
- Conectar buffer USB (`spk_buf`) a salida I2S
- Cambio dinamico de sample rate (reconfigurar I2S al cambiar clock UAC2)

### F2 - Display & UI Base
- Inicializacion MIPI DSI para panel 720x1280
- Integracion LVGL como framework de UI
- Driver I2C del touch panel
- UI basica: pantalla now-playing, volumen, indicador de sample rate

### F3 - Hardware EQ / DSP
- Procesamiento EQ por software en el pipeline de audio
- Aplicar EQ entre lectura USB FIFO y escritura I2S
- Presets de EQ configurables por el usuario via UI

### F4 - Gestion de Energia
- Driver I2C del MAX77972 (cargador + fuel gauge)
- Monitorizacion de nivel de bateria y display en UI
- Maquina de estados de carga (CC/CV/completo/error)
- Driver BMA400 (orientacion, gestos)
- Control pin HW shutdown del ESP32-P4
- Wake RTC desde VBAT (cristal 32 kHz)

### F5 - Controles Fisicos
- Driver GPIO para 5 botones con debounce
- Deteccion pulsacion corta / larga
- Integracion con pipeline de audio (volumen, transporte)

### F6 - Reproduccion microSD
- Driver SDIO para microSD
- Filesystem FAT/exFAT
- Decoders de audio: FLAC, WAV, MP3 (minimo)
- USB MSC para transferencia de archivos
- Gestion de playlists / biblioteca

### F7 - Wireless ESP32-C5
- Comunicacion SDIO con C5
- Streaming BT5 (modo source para auriculares)
- WiFi6 para app companion y OTA
- Diseno del protocolo P4 <-> C5

### F8 - UI Avanzada
- Album art en pantalla
- Transiciones animadas entre pantallas
- Menus de configuracion (EQ, BT, WiFi, bateria)
- Navegador de archivos microSD

### F9 - Polish y Features Avanzados
- DSD over PCM (DoP): deteccion y cambio de modo del DAC
- Gapless playback
- Testing de compliance USB Audio Class
- Optimizacion de consumo
- OTA firmware updates via C5

---

## 9. Build y Flash

### Requisitos
- ESP-IDF v5.5.2+
- Target: `esp32p4`

### Comandos
```bash
idf.py set-target esp32p4
idf.py build
idf.py flash
idf.py monitor
```

### Dependencias Gestionadas
- `espressif/tinyusb >=0.19.0~2` (via idf_component.yml)

### Notas de Build
- `MINIMAL_BUILD ON` en CMakeLists.txt raiz: solo compila componentes referenciados
- Los callbacks de descriptores TinyUSB requieren flags `-u` en el linker
- Los callbacks de audio/CDC son `TU_ATTR_WEAK` y no necesitan flags `-u`