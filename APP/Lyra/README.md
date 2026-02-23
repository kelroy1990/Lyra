# Lyra - Hi-Res USB DAC & Audio Player

Reproductor de audio portatil de alta resolucion basado en ESP32-P4 con salida balanceada 4.4mm, pantalla tactil y conectividad inalambrica.

---

## Indice

1. [Hardware](#1-hardware)
2. [Arquitectura de Software](#2-arquitectura-de-software)
3. [Estructura del Proyecto](#3-estructura-del-proyecto)
4. [Implementacion USB](#4-implementacion-usb)
5. [Pipeline de Audio](#5-pipeline-de-audio)
6. [Formatos Soportados](#6-formatos-soportados)
7. [Sistema de Tareas (FreeRTOS)](#7-sistema-de-tareas-freertos)
8. [Presupuesto de Memoria](#8-presupuesto-de-memoria)
9. [Fases de Desarrollo](#9-fases-de-desarrollo)
10. [Build y Flash](#10-build-y-flash)

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
| DSD               | DoP hasta DSD256 (45.2 MHz BCLK)                     |
| MCLK              | APLL auto-config, max 50 MHz (DAC limit)              |
| Salida            | Jack 4.4mm balanceado con deteccion GPIO              |
| Alimentacion      | LDO dedicado para +-10V limpio (EN por GPIO)          |
| I2S GPIOs (final) | DOUT=10, LRCK=11, BCLK=12, MCLK=13                  |

### 1.3 Pantalla

| Parametro    | Valor                          |
|--------------|--------------------------------|
| Resolucion   | 720 x 1280 (portrait)         |
| Tamaño       | 5 pulgadas                     |
| Interfaz     | MIPI DSI (PPI nativo del P4)   |
| Touch        | Panel tactil I2C               |

### 1.4 Almacenamiento

| Medio    | Interfaz | Velocidad | Notas                      |
|----------|----------|-----------|----------------------------|
| microSD  | SDIO Slot 1 (GPIO Matrix) | SDR50 100MHz UHS-I | FAT32/exFAT, audio local |

### 1.5 Conectividad Inalambrica

| Componente | Interfaz | Capacidades | Board |
|------------|----------|-------------|-------|
| ESP32-C6-MINI-1 | SDIO | WiFi 6 + BT 5.0 | Dev board |
| ESP32-C5-WROOM-1U-N8R8 | SDIO | WiFi 5/6 + BT 5.0 | Placa final |

Via esp_hosted SDIO slave. Uso: streaming HTTP, Spotify Connect, BT a auriculares, OTA.

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
| SDIO       | Slot 1 | microSD       | Almacenamiento (GPIO Matrix, SDR50 100MHz) |
| SDIO       | Slot 1 | ESP32-C5/C6   | Companion inalambrico (esp_hosted) |
| MIPI DSI   | PPI   | Display        | 720x1280 portrait        |
| USB OTG    | HS    | Host PC        | UAC2 + CDC + MSC         |
| GPIO       | -     | Botones (5)    | Controles de usuario     |

---

## 2. Arquitectura de Software

### 2.1 Diagrama de Bloques

```
+------------------+     +-------------------+     +-------------------+
|   USB Host (PC)  |     |   microSD (SDIO)  |     |  HTTP/Internet    |
+--------+---------+     +---------+---------+     +---------+---------+
         |                         |                         |
    USB OTG 2.0 HS           SDIO SDR50              esp_hosted WiFi
         |                    100MHz UHS-I            (ESP32-C6/C5)
+--------v---------+     +---------v---------+     +---------v---------+
|   usb_device     |     |     storage       |     |    net_audio      |
|  UAC2+CDC+MSC    |     |  FAT32/exFAT FS   |     |  HTTP/HTTPS stream|
+--------+---------+     +---------+---------+     |  ICY metadata     |
         |                         |                +---------+---------+
         +-------+     +----------+     +--------------------+
                 |     |                |
          +------v-----v----------------v---+
          |        audio_source manager     |
          |     (USB / SD / NET switch)     |
          +--------+------------------------+
                   |
          +--------v----------+
          |   DSP / EQ Chain  |
          |  Biquad IIR (FPU) |
          +--------+----------+
                   |
          +--------v----------+
          |   StreamBuffer    |
          |     (16 KB)       |
          +--------+----------+
                   |
          +--------v----------+
          |   I2S DMA Output  |
          |   (APLL clock)    |
          +--------+----------+
                   |
          +--------v----------+
          |    ES9039Q2M DAC  |
          +--------+----------+
                   |
          +--------v----------+
          | 4.4mm Balanced Out|
          +-------------------+
```

### 2.2 Dispositivo USB Compuesto

```
Host PC detecta:
  IAD #1 : UAC2 Speaker    (ITF 0 Audio Control, ITF 1 Audio Streaming)
  IAD #2 : CDC ACM Serial  (ITF 2 CDC Control,   ITF 3 CDC Data)
  IAD #3 : MSC             (ITF 4 Mass Storage)
```

### 2.3 Flujo de Audio USB (UAC2 Asincrono)

1. Host establece sample rate via Clock Set Request
2. Host abre streaming interface (alt setting 1)
3. Host envia audio PCM por EP OUT isocrono
4. `audio_task` lee del FIFO TinyUSB cada 1 ms via `tud_audio_read()`
5. Feedback EP reporta nivel del FIFO (`AUDIO_FEEDBACK_METHOD_FIFO_COUNT`)
6. Audio se envia a I2S DMA → ES9039Q2M

### 2.4 Flujo CDC (printf sobre USB)

```
printf() → stdout → freopen("/dev/usbcdc") → cdc_vfs_write() → tud_cdc_write() → USB CDC IN
```

---

## 3. Estructura del Proyecto

```
lyra/
├── CMakeLists.txt                          # Raiz del proyecto ESP-IDF
├── README.md                               # Este archivo
├── TODO.md                                 # Tracking de tareas y estado
├── DSP_BUDGET_GUIDE.md                     # Guia integración UI con budget DSP
├── partitions.csv                          # Tabla de particiones (factory 3MB)
├── sdkconfig / sdkconfig.defaults          # Configuración ESP-IDF
├── main/
│   ├── app_main.c                          # Entry point, I2S, audio/feeder tasks, CDC
│   ├── audio_source.c/.h                   # Audio source manager (USB/SD/NET switch)
│   ├── tusb_config.h                       # Configuración TinyUSB
│   ├── usb_descriptors.c/.h                # Callbacks de descriptores USB
│   ├── usb_msc.c                           # USB Mass Storage Class
│   └── usb_mode.c/.h                       # USB mode switching (Audio/MSC)
└── components/
    ├── audio_pipeline/                     # DSP chain, biquad, presets
    │   ├── audio_pipeline.c                # Integration layer
    │   ├── dsp_biquad.c/.h                 # Biquad IIR filters (FPU optimized)
    │   ├── dsp_chain.c/.h                  # DSP chain manager + budget API
    │   ├── dsp_presets.c/.h                # 7 presets + dynamic coefficients
    │   └── include/dsp_types.h             # Tipos comunes DSP
    ├── audio_codecs/                       # 9 formatos de audio
    │   ├── audio_codecs.c                  # Codec dispatcher (format detect, setvbuf 32KB)
    │   ├── include/audio_codecs.h          # Public API + codec_info_t (gain_db)
    │   ├── audio_codecs_internal.h         # Internal handle + vtable (extern "C" guards)
    │   ├── codec_wav.c                     # WAV/AIFF (dr_wav, mono→stereo)
    │   ├── codec_flac.c                    # FLAC (dr_flac, ReplayGain pre-scan)
    │   ├── codec_mp3.c                     # MP3 (dr_mp3 / minimp3)
    │   ├── codec_aac.c                     # AAC ADTS + AAC-M4A (opencore-aacdec)
    │   ├── codec_alac.cpp                  # ALAC M4A (Apple ALACDecoder C++)
    │   ├── codec_opus.c                    # Opus/Ogg (libopus, seek, R128 gain)
    │   ├── codec_dsd.c                     # DSF + DFF/DSDIFF → DoP
    │   ├── m4a_demuxer.c/.h                # ISO BMFF parser (moov/trak/stbl)
    │   └── third_party/                    # dr_wav, dr_flac, dr_mp3 headers
    ├── sd_player/                          # SD card audio playback
    │   ├── sd_player.c                     # Player engine, ReplayGain Q16, CDC cmds
    │   ├── sd_playlist.c                   # Playlist scan (.wav .flac .mp3 .aac .opus
    │   │                                   #   .dsf .dff .m4a .m4b), CUE dedup
    │   ├── cue_parser.c/.h                 # CUE sheet parser (single-FILE)
    │   └── include/sd_player.h             # Public API
    ├── net_audio/                          # HTTP/HTTPS audio streaming
    │   ├── net_audio.c                     # HTTP client, codec detect, ICY, pre-buffer
    │   └── include/net_audio.h             # Public API + net_audio_info_t
    ├── dlna/                               # DLNA/UPnP renderer (stub)
    │   └── dlna.c
    ├── spotify/                            # Spotify Connect via cspot
    │   ├── spotify.c                       # ESP-IDF glue
    │   ├── spotify_glue.cpp                # C++→C bridge
    │   └── vendor/cspot                    # cspot submodule
    ├── storage/                            # microSD (SDMMC) + filesystem
    │   ├── sd_card.c                       # SDMMC driver, UHS-I SDR50, fallback chain
    │   └── include/storage.h
    ├── tinyusb/                            # TinyUSB local build
    │   └── CMakeLists.txt                  # DWC2 slave, rhport1 HS
    ├── display/                            # MIPI DSI driver + LVGL (stub)
    ├── ui/                                 # Pantallas, menus (stub)
    ├── input/                              # GPIO buttons (stub)
    ├── power/                              # MAX77972 + bateria (stub)
    ├── sensors/                            # BMA400 acelerometro (stub)
    └── wireless/                           # ESP32-C5 config (stub)
```

---

## 4. Implementacion USB

### 4.1 Configuracion de Audio (tusb_config.h)

| Parametro                        | Valor    |
|----------------------------------|----------|
| Sample rate maximo               | 384 kHz  |
| Canales RX                       | 2 (stereo)|
| Bytes por muestra                | 4 (32-bit)|
| Resolucion                       | 32 bits  |
| EP OUT SW buffer                 | 32× EP size max |
| Feedback EP                      | Habilitado (asincrono) |
| Modo USB                         | High-Speed (480 Mbps)  |
| rhport                           | 1 (UTMI PHY, HS controller) |

### 4.2 Entidades UAC2

| Entity ID | Tipo              | Descripcion            |
|-----------|-------------------|------------------------|
| 0x04      | Clock Source      | Reloj interno programable |
| 0x01      | Input Terminal    | USB Streaming input    |
| 0x02      | Feature Unit      | Mute + Volume (per-ch) |
| 0x03      | Output Terminal   | Desktop Speaker        |

### 4.3 Interfaces USB

| Interface | Uso                | String Descriptor |
|-----------|--------------------|-------------------|
| ITF 0     | Audio Control      | "UAC2 Speaker"    |
| ITF 1     | Audio Streaming    | -                 |
| ITF 2     | CDC ACM Control    | "CDC Serial"      |
| ITF 3     | CDC Data           | -                 |
| ITF 4     | Mass Storage       | "MSC SD Card"     |

---

## 5. Pipeline de Audio

```
USB Host ──→ TinyUSB FIFO (12.5KB) ──→ audio_task (DSP) ──┐
                ↑                                          │
                └── async feedback ←── FIFO level          │
                                                           ↓
SD Card ──→ sd_player_task (decode) ──→ audio_source ──→ StreamBuffer (16KB)
        9 codecs, setvbuf 32KB           manager              │
        1024 frames/block, ReplayGain  (USB/SD/NET)           ↓
                                       i2s_output_init   i2s_feeder_task
HTTP ──→ net_audio_task (stream) ──┘   (actual_rate)          │
        MP3/FLAC/WAV/AAC/Ogg                                  ↓
        ICY metadata, HTTPS                            I2S DMA → DAC
        pre-buffer, pause/resume                              │
                                                              ↓
                                                    4.4mm Balanced Output
```

### 5.1 Control del DAC (ES9039Q2M)

| Señal         | Tipo | Funcion                              |
|---------------|------|--------------------------------------|
| DAC EN        | GPIO | Habilitar/deshabilitar el ES9039Q2M  |
| Audio LDO EN  | GPIO | Habilitar alimentacion +-10V limpia  |
| Jack Detect   | GPIO | Detectar conexion jack 4.4mm         |
| SPI           | Bus  | Configurar registros del DAC         |
| I2S           | Bus  | Transmitir datos PCM/DoP al DAC      |

### 5.2 DSD over PCM (DoP)

ES9039Q2M detecta DoP markers (0x05/0xFA en bits [23:16]) automaticamente.

| DSD Rate | I2S Sample Rate | BCLK | Estado |
|----------|----------------|------|--------|
| DSD64 | 176.4 kHz | 11.3 MHz | Soportado |
| DSD128 | 352.8 kHz | 22.6 MHz | Soportado |
| DSD256 | 705.6 kHz | 45.2 MHz | Feasible (APLL limit) |
| DSD512 | 1.41 MHz | 90.3 MHz | No viable (>50 MHz DAC limit) |

---

## 6. Formatos Soportados

### 6.1 SD Card Playback

| Formato | Decoder | Max Resolution | Seek | ReplayGain | Extensiones |
|---------|---------|---------------|------|------------|-------------|
| WAV/AIFF | dr_wav | 384kHz/32-bit | Exacto | — | .wav .aiff |
| FLAC | dr_flac | 384kHz/32-bit | Exacto | REPLAYGAIN_TRACK_GAIN | .flac |
| MP3 | minimp3 | 320kbps | Aproximado | — | .mp3 |
| AAC (ADTS) | opencore-aacdec | AAC-LC/HE-AAC | Aproximado | — | .aac |
| AAC (M4A) | opencore-aacdec | AAC-LC/HE-AAC | Exacto | — | .m4a .m4b |
| ALAC | Apple ALACDecoder | 384kHz/32-bit | Exacto | — | .m4a .m4b |
| Opus | libopus | 48kHz | Aproximado | R128_TRACK_GAIN | .opus |
| DSF | Custom parser | DSD256 (DoP) | Lineal | — | .dsf |
| DFF | Custom parser | DSD256 (DoP) | Lineal | — | .dff |

### 6.2 HTTP Streaming

MP3, FLAC, WAV, AAC, Ogg via Content-Type autodetect o codec hint.
ICY metadata (StreamTitle) para internet radio. HTTPS soportado.

### 6.3 USB Audio (UAC2)

PCM 16/24/32-bit stereo, 44.1 kHz – 384 kHz, asincrono con feedback EP.

---

## 7. Sistema de Tareas (FreeRTOS)

| Tarea             | Prioridad | Core | Stack | Funcion                           |
|-------------------|-----------|------|-------|-----------------------------------|
| tusb_device_task  | 5         | 1    | 4KB   | Manejo de eventos USB TinyUSB     |
| audio_task        | 5         | 1    | 12KB  | USB FIFO → DSP → StreamBuffer     |
| i2s_feeder_task   | 4         | 1    | 8KB   | StreamBuffer → I2S DMA → DAC      |
| sd_player_task    | 3         | 0    | 8KB   | SD decode → StreamBuffer          |
| net_audio_task    | 3         | 0    | 8KB   | HTTP stream → decode → StreamBuffer|
| msc_io_task       | 3         | 1    | 4KB   | MSC double-buffer ping-pong       |
| ui_task           | 3         | 0    | -     | LVGL display + touch (futuro)     |

**Core 0:** UI + sistema + SD decode + HTTP streaming (no tiempo real)
**Core 1:** USB + audio pipeline + I2S (tiempo real, baja latencia)

---

## 8. Presupuesto de Memoria

### 8.1 SRAM Interna (768 KB)

| Uso                        | Estimacion |
|----------------------------|------------|
| FreeRTOS kernel + stacks   | ~80 KB     |
| USB EP buffers + FIFO      | ~16 KB     |
| I2S DMA buffers            | ~8 KB      |
| TinyUSB internal           | ~12 KB     |
| TCP/IP (lwIP + esp_hosted) | ~40 KB     |
| Variables globales / BSS   | ~20 KB     |
| **Disponible**             | **~592 KB**|

### 8.2 PSRAM (32 MB)

| Uso                           | Estimacion |
|-------------------------------|------------|
| Framebuffer display (720x1280x2) | ~1.8 MB |
| LVGL working buffers          | ~1.8 MB    |
| Audio stream buffer           | ~16 KB     |
| SD setvbuf + decode buffers   | ~64 KB     |
| M4A sample tables (per file)  | ~500 KB    |
| HTTP streaming buffers        | ~32 KB     |
| LVGL objects + themes         | ~1 MB      |
| **Disponible**                | **~27 MB** |

### 8.3 Flash (8 MB min)

| Uso                   | Tamaño     |
|-----------------------|------------|
| Firmware (factory)    | ~2.3 MB (partition 3 MB) |
| LVGL assets + fonts   | ~2-4 MB (futuro) |
| NVS (configuracion)   | 24 KB      |
| PHY init              | 4 KB       |

---

## 9. Fases de Desarrollo

| Fase | Nombre                    | Estado       |
|------|---------------------------|--------------|
| F0   | Estructura del proyecto   | ✅ Completado |
| F0.5 | USB Audio (UAC2+CDC)      | ✅ Completado |
| F1   | I2S output a DAC          | 🟡 Temporal (ES8311) |
| F1.5 | USB Mass Storage (MSC)    | ✅ Completado (13.5/8.7 MB/s R/W) |
| F2   | Display & UI base         | ⏸️ Próximo (simulador Python) |
| F3   | EQ / DSP Pipeline         | ✅ Completado |
| F3.5 | DSP Features avanzadas    | ⏸️ Pendiente |
| F4   | Gestion de energia        | ⏸️ Pendiente |
| F5   | Controles fisicos         | ⏸️ Pendiente |
| F6   | Reproduccion microSD      | ✅ Completado (9 formatos) |
| F7-A | WiFi (esp_hosted C6)      | ✅ Completado |
| F7-B | Wireless (C5 final)       | ⏸️ Pendiente |
| F8-A | HTTP streaming            | ✅ Completado |
| F8-B | UI avanzada               | ⏸️ Pendiente |
| F9-A | Spotify Connect           | 🟡 En progreso |
| HW   | Placa final (ES9039Q2M)   | ⏸️ Pendiente |

---

## 10. Build y Flash

### Requisitos
- ESP-IDF v5.5.2+
- Target: `esp32p4`

### Comandos
```bash
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

### Comandos CDC (puerto serial USB)
```
# Audio
play [file]      # Reproducir archivo SD
stop             # Parar reproducción
next / prev      # Siguiente / anterior pista
seek <seconds>   # Buscar posición
playlist         # Listar pistas

# EQ / DSP
rock / jazz / classical / bass / flat / test
on / off         # Enable/disable DSP
status           # Info actual

# WiFi
wifi connect <SSID> <PASS>
wifi status

# HTTP Streaming
radio <url> [codec_hint] [referer]
radio stop

# SD Card
sd ls [path]     # Listar archivos
sd info          # Info tarjeta SD
sd format        # Formatear (FAT32/exFAT auto)

# USB
msc              # Entrar modo Mass Storage
audio            # Volver a modo Audio
```

### Dependencias
- `espressif/tinyusb >=0.19.0~2`
- `espressif/esp_hosted` (SDIO slave)
- `espressif/libopus`
- `cspot` (submodule, Spotify Connect)
- Apple ALAC decoder (via cspot/bell/external/alac)
- opencore-aacdec (via cspot/bell/external/opencore-aacdec)
