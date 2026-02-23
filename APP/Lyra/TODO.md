# TODO - Lyra Project

> **Última actualización:** 2026-02-23
> **Estado actual:** Audio pipeline completo (USB + SD + NET + DSP + I2S), 9 formatos soportados, WiFi operativo, Spotify en integración. Falta UI y hardware final.

---

## Estado General del Proyecto

### Fases Completadas

- **F0**: Estructura del proyecto
- **F0.5**: USB Audio (UAC2 + CDC)
- **F1**: I2S output (temporal con ES8311, objetivo ES9039Q2M)
- **F1.5**: MSC (USB Mass Storage) — **READ ~13.5 MB/s, WRITE ~8.7 MB/s** (SDR50 100MHz UHS-I, double-buffer ping-pong)
- **F3**: DSP Pipeline con EQ (biquad IIR, FPU optimized, budget management)
- **F3.1**: Audio Pipeline decoupled architecture — space-check, zero overflow
- **F3.2**: Fix coeficientes biquad — eliminado path pre-calculado con error 2x
- **F6**: Reproducción microSD — WAV/FLAC/MP3, playlist, CUE parser, audio_source manager
- **F6.1**: I2S reconfig entre pistas SD — same-source format change + fallback rate propagation
- **F6.2**: SD throughput — setvbuf 32KB, decode block 1024 frames, SD CRC safety check
- **F6.3**: CUE sheet parser — implementado (sin testear, falta .cue de prueba)
- **F6.4**: **Codecs completos** — AAC (ADTS + M4A), ALAC, Opus (seek + R128 gain), DSD (DSF + DFF/DSDIFF), FLAC ReplayGain
- **F7-A**: **WiFi operativo** — esp_hosted SDIO + ESP32-C6 companion (dev board)
- **F8-A**: **HTTP streaming** — MP3/FLAC/WAV/AAC/Ogg, ICY metadata, HTTPS, Referer

### En Progreso

- **F9-A**: **Spotify Connect** — cspot integration (en branch, parcialmente funcional)

### Próximas Fases

- **F2**: Display & UI base (LVGL + MIPI DSI) ← **PRÓXIMO**
  - Simulador Python para desarrollo sin hardware
  - Pantalla 720×1280 portrait (5")
- **F3.5**: DSP Features avanzadas (EQ paramétrico 5 bandas, crossfeed, loudness)
- **F4**: Gestión de energía (MAX77972, BMA400)
- **F5**: Controles físicos (botones GPIO)
- **F7-B**: ESP32-C5 wireless (placa final, WiFi 5/6 + BT5)
- **F8-B**: UI avanzada (album art, navegador, config)
- **HW**: Migración a ES9039Q2M (placa final)

---

## Formatos de Audio Soportados

### SD Card Playback

| Formato | Contenedor | Codec | Seek | ReplayGain | Extensiones |
|---------|-----------|-------|------|------------|-------------|
| WAV | WAV/AIFF | PCM/float/ADPCM | Exacto | — | .wav .aiff |
| FLAC | FLAC nativo | FLAC | Exacto | Vorbis Comment | .flac |
| MP3 | MP3 | MP3 (minimp3) | Aproximado | — | .mp3 |
| AAC | ADTS | AAC-LC / HE-AAC | Aproximado | — | .aac |
| AAC | M4A (ISO BMFF) | AAC-LC / HE-AAC | Exacto (sample table) | — | .m4a .m4b |
| ALAC | M4A (ISO BMFF) | Apple Lossless | Exacto (sample table) | — | .m4a .m4b |
| Opus | Ogg | Opus | Aproximado (interpolación) | R128_TRACK_GAIN | .opus |
| DSF | DSF (Sony) | DSD→DoP | Lineal | — | .dsf |
| DFF | DSDIFF (Philips) | DSD→DoP | Lineal | — | .dff |

### HTTP Streaming (net_audio)

| Formato | Detección | Notas |
|---------|-----------|-------|
| MP3 | Content-Type / codec_hint | ICY metadata (StreamTitle) |
| FLAC | Content-Type / codec_hint | |
| WAV | Content-Type / codec_hint | |
| AAC | Content-Type / codec_hint | ADTS streams |
| Ogg | Content-Type / codec_hint | Vorbis/Opus |

### USB Audio (UAC2)

| Parámetro | Valor |
|-----------|-------|
| Sample rates | 44.1k – 384 kHz |
| Bit depth | 16/24/32-bit |
| Canales | Stereo |
| Modo | Asíncrono (feedback EP) |
| DSD | DoP automático (DAC detecta markers) |

---

## Detalles por Fase

### F6.4 — Codecs Completos (COMPLETADA)

**M4A Demuxer** (`m4a_demuxer.c/.h`):
- Parser ISO BMFF completo: moov → trak → mdia → stbl
- Reconstruye tabla de samples plana (offsets + sizes) desde stsc/stsz/stco/co64
- Detecta codec por stsd box: `mp4a` → AAC, `alac` → ALAC
- Extrae AudioSpecificConfig (AAC) o ALACSpecificConfig (magic cookie)
- Memoria: ~12 bytes/frame en PSRAM (1h ALAC 48kHz ≈ 500 KB)

**ALAC** (`codec_alac.cpp`):
- Apple ALACDecoder C++ (del repositorio cspot/bell/external/alac/codec)
- Decodifica 16/24/32-bit Apple Lossless
- Seek exacto por sample table index
- `extern "C"` wrapper para integración con dispatcher C

**AAC-M4A** (`codec_aac.c` extendido):
- Reutiliza opencore-aacdec existente
- Frames raw (sin ADTS header) por sample table
- AudioSpecificConfig via `PVMP4AudioDecoderConfig()`
- Seek exacto por sample table index

**DFF/DSDIFF** (`codec_dsd.c` extendido):
- Parser big-endian IFF (FRM8 → DSD/PROP/DST chunks)
- Interleaved L0,R0,L1,R1 → DoP packing con markers 0x05/0xFA
- Seek lineal: `data_offset + frame_pos * 4`

**Opus seek + R128** (`codec_opus.c` extendido):
- Seek aproximado: interpolación lineal por file_size + forward Ogg scan
- Duración total: scan últimos 64KB → last page granule position
- R128_TRACK_GAIN: parse OpusTags Vorbis comment, int16 Q7.8 → float dB

**FLAC ReplayGain** (`codec_flac.c` extendido):
- Pre-scan de metadata blocks antes de `drflac_open()`
- Lee tipo 4 (VORBIS_COMMENT), busca `REPLAYGAIN_TRACK_GAIN=`
- `strtof()` para valor dB, rewind antes de abrir decoder

**ReplayGain aplicación** (`sd_player.c`):
- `gain_db` en `codec_info_t` (0.0 = sin ajuste)
- Conversión a Q16: `powf(10, gain_db/20) * 65536`
- Aplicación int64 con saturación INT32_MAX/MIN antes de DSP chain
- Cache estático: solo recalcula cuando cambia gain_db

### F7-A — WiFi Operativo (COMPLETADA)

**Hardware dev board:** ESP32-C6-MINI-1 via SDIO (esp_hosted)
**Hardware placa final:** ESP32-C5-WROOM-1U-N8R8

- esp_hosted SDIO slave: DAT0-3=GPIO14-17, CLK=18, CMD=19
- Reset GPIO=54, Boot GPIO=53 (final only)
- DHCP funcional con `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP=y`
- TCP window: 32KB max (DRAM constraint)
- DNS: lwip_getaddrinfo() + manual DNS1 fallback
- Conexión via CDC: `wifi connect <SSID> <PASS>`

### F8-A — HTTP Streaming (COMPLETADA)

**Componente:** `components/net_audio/`

- HTTP/HTTPS streaming con esp_http_client
- Codecs: MP3, FLAC, WAV, AAC, Ogg (autodetect por Content-Type o hint)
- ICY metadata: parsea StreamTitle de internet radio
- Referer header configurable (anti-hotlink para Radio France, BBC, etc.)
- Pre-buffering antes de reproducción
- Pause/resume sin cerrar conexión HTTP
- Audio source manager: conmutación USB ↔ SD ↔ NET
- DAC mute callback para transiciones click-free
- Comandos CDC: `radio <url>`, `radio stop`

---

## TODOs Pendientes

### Prioridad ALTA — F2 (Display & UI)

- [ ] **Simulador Python** para desarrollo UI sin hardware
  - Framework: pygame/SDL o tkinter con canvas 720×1280
  - Renderizar elementos C (botones, listas, waveforms) con datos mock
  - Hot-reload de cambios
- [ ] **LVGL integration** (cuando llegue hardware)
  - MIPI DSI driver para panel 5" 720×1280
  - Touch I2C driver
  - Pantalla Now Playing (título, artista, formato, sample rate, progress bar)
  - Navegador de archivos SD
  - Selector de EQ / presets DSP

### Prioridad ALTA — F3.5 (DSP Features)

- [ ] **EQ Paramétrico de 5 bandas (usuario configurable)**
  - 5 filtros biquad: Low Shelf, 3× Peaking, High Shelf
  - Frecuencias default: 60Hz, 230Hz, 1kHz, 3.5kHz, 12kHz
  - Ganancia: -12/+12 dB, Q: 0.1-10
  - Coste: 5 × 18 = 90 cycles (trivial incluso @ 384kHz)

- [ ] **Crossfeed** para auriculares
  - Algoritmo: Chu Moy o Jan Meier
  - Parámetro: intensidad (0-100%, default ~30%)
  - Coste: ~100 cycles

- [ ] **Loudness Compensation** (Fletcher-Munson)
  - Boost graves/agudos a volumen bajo (ISO 226)
  - 3-4 filtros × 18 = 54-72 cycles

### Prioridad MEDIA

- [ ] **Balance L/R** — trivial, ~4 cycles
- [ ] **Selección filtro digital DAC** — ES9039Q2M tiene 7 presets via SPI/I2C
- [ ] **NVS Storage** para presets personalizados (5-10 slots)
- [ ] **Más presets** predefinidos (Pop, Metal, Electronic, Vocal, Acoustic)
- [ ] **CUE sheet testing** (falta archivo .cue de prueba)
- [ ] **DLNA/UPnP renderer** (componente creado, pendiente)
- [ ] **Spotify Connect** (cspot integrado, en progreso)

### Prioridad BAJA (Futuro)

- [ ] **DRC** (Dynamic Range Compression) — solo viable @ ≤192kHz
- [ ] **Room correction** offline (pre-procesar en app companion)
- [ ] **Gapless playback** entre pistas
- [ ] **OTA firmware updates** via WiFi

### Hardware (Placa Final)

- [ ] **ES9039Q2M DAC** — driver SPI, filtros, THD compensation
- [ ] **MIPI DSI display** 720×1280
- [ ] **Touch panel** I2C
- [ ] **MAX77972** cargador + fuel gauge
- [ ] **BMA400** acelerómetro
- [ ] **5 botones GPIO** con debounce
- [ ] **ESP32-C5** migración desde C6
- [ ] **Jack 4.4mm** detección GPIO

---

## Issues Conocidos

### RESUELTO: Audio stuttering con DSP
- **Causa**: Buffer conversion overhead
- **Solución**: Frame-by-frame + ILP optimization

### RESUELTO: EQ no audible
- **Causa**: Gain insuficiente
- **Solución**: Presets extremos (+12dB, +20dB)

### RESUELTO: Stream buffer overflow
- **Causa**: Non-blocking writes + no flow control
- **Solución**: Space-check architecture + task notification

### RESUELTO: Coeficientes biquad 2x
- **Causa**: Pre-calculated coeffs tenían b0/b1/b2 duplicados
- **Solución**: Siempre usar cálculo dinámico

### RESUELTO: I2S no reconfig entre pistas SD
- **Causa**: `audio_source_switch()` early return cuando source==source
- **Solución**: Comparar formato, no solo source

### RESUELTO: DHCP no funciona via esp_hosted
- **Causa**: ACD ARP probe broadcast unreliable via SDIO → DECLINE
- **Solución**: `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP=y`

### RESUELTO: esp_hosted SDIO crash (DRAM OOM)
- **Causa**: TCP window 256KB → 170+ pbufs en internal DRAM
- **Solución**: `TCP_WND_DEFAULT=32768`, `WND_SCALE=n`

### RESUELTO: DDR50 data corruption on GPIO Matrix
- **Causa**: GPIO Matrix skew (~5-10 ns) + no tuning
- **Solución**: SDR50 100MHz con CMD19 tuning

### RESUELTO: Partition overflow con codecs adicionales
- **Causa**: Binary 2.3 MB > factory partition 2 MB
- **Solución**: Factory partition 2 MB → 3 MB

### CONOCIDO: ES8311 no soporta >96kHz bien
- **Causa**: MCLK 49.15 MHz excede límite ES8311 (~25 MHz)
- **Estado**: Limitación dev board. ES9039Q2M soporta hasta 50 MHz.

### CONOCIDO: CUE sheet sin testear
- **Estado**: Parser implementado, falta archivo .cue de prueba

---

## Arquitectura Audio Pipeline (Actual)

```
USB Host ──→ TinyUSB FIFO (12.5KB) ──→ audio_task (DSP) ──┐
                ↑                                          │
                └── async feedback ←── FIFO level          │
                                                           ↓
SD Card ──→ sd_player_task (decode) ──→ audio_source ──→ StreamBuffer (16KB)
        9 codecs (WAV/FLAC/MP3/         manager              │
        AAC/ALAC/Opus/DSD/M4A)      (USB/SD/NET switch)      ↓
        setvbuf 32KB, 1024 fr/blk   i2s_output_init     i2s_feeder_task
        ReplayGain Q16               (actual_rate)            │
                                                              ↓
HTTP ──→ net_audio_task (stream) ──→ audio_source ──→  I2S DMA → DAC
        MP3/FLAC/WAV/AAC/Ogg           manager               │
        ICY metadata, HTTPS                                   ↓
        pre-buffer, pause/resume            4.4mm Balanced Output
```

**Tasks:**
- **audio_task** (prio 5, core 1, 12KB): USB FIFO → DSP → StreamBuffer
- **i2s_feeder_task** (prio 4, core 1, 8KB): StreamBuffer → I2S DMA → DAC
- **sd_player_task** (prio 3, core 0, 8KB): SD decode → StreamBuffer
- **net_audio_task** (prio 3, core 0, 8KB): HTTP stream → decode → StreamBuffer
- **tusb_device_task** (prio 5, core 1): TinyUSB USB events

---

## Notas Técnicas

### Cycle Budget @ 384kHz

```
CPU: 400 MHz / (384 kHz × 2 ch) = 1042 cycles/sample
Safety (85%):                     885 cycles/sample
Base overhead:                    -34 cycles (conversión + limiter)
Available for filters:            851 cycles
Max: 851 / 18 = 47 filtros teórico, 25-30 recomendado
```

### MSC Performance (SDR50 100MHz UHS-I)

| Operación | Velocidad | Notas |
|-----------|-----------|-------|
| READ | ~13.5 MB/s | Bottleneck: DWC2 slave mode |
| WRITE | ~8.7 MB/s avg (peak 11.1) | Bottleneck: NAND flash |

### Memory Budget

- **Internal DRAM** (~768 KB): DMA buffers, TCP/IP, FreeRTOS stacks
- **PSRAM** (32 MB): Audio buffers, LVGL framebuffers, sample tables, decode state
- **Flash** (8 MB min): Firmware ~2.3 MB, LVGL assets ~2-4 MB, NVS 64 KB
- **Factory partition**: 3 MB (0x300000)

### SDMMC Speed Modes

| Mode | Freq | Voltage | Status |
|------|------|---------|--------|
| SDR50 100MHz | 100 MHz | 1.8V UHS-I | **Production** |
| SDR 40MHz HS | 40 MHz | 3.3V | Fallback |
| DDR50 | Any | Any | BROKEN (GPIO Matrix) |

---

**Fin del TODO — Actualizar según progreso**
