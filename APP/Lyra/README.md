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

### 6.1 Arquitectura de Latencia: Polling vs Interrupciones

**Implementación actual:** Polling con `taskYIELD()`

```c
while (1) {
    if (tud_audio_available() > 0) {
        process_audio();  // ~5-10 μs
    } else {
        taskYIELD();      // ~10-50 μs (context switch)
    }
}
```

**Latencia medida:** 10-50 μs (depende del scheduler)

#### Alternativa: Modelo basado en interrupciones

**Enfoque 1: Interrupción I2S DMA**
```c
// ISR cuando DMA buffer está medio vacío
void i2s_dma_isr(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(audio_task_handle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// audio_task esperando notificación
void audio_task(void *arg) {
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Suspendida hasta IRQ
        process_audio();
    }
}
```

**Enfoque 2: Interrupción USB SOF (Start of Frame)**
```c
// TinyUSB callback cada 1ms (HS) o 125μs (HS microframe)
void tud_sof_cb(uint32_t frame_count) {
    xTaskNotifyGive(audio_task_handle);
}
```

#### Comparación de enfoques

| Aspecto | Polling (actual) | IRQ I2S DMA | IRQ USB SOF |
|---------|-----------------|-------------|-------------|
| **Latencia** | 10-50 μs | 1-5 μs | 125 μs (HS) / 1 ms (FS) |
| **Jitter** | Medio (~20 μs) | Bajo (~1 μs) | Muy bajo (<1 μs) |
| **CPU usage (idle)** | ~5-10% | <1% | <1% |
| **CPU usage (streaming)** | ~5% | ~5% | ~5% |
| **Complejidad** | Baja ✅ | Media | Media |
| **Determinismo** | Bajo | Alto ✅ | Muy alto ✅ |

#### Análisis de viabilidad

**¿Latencia de 0 μs es necesaria?**

No. Análisis del pipeline completo:

```
USB Host → USB Bus → ESP32 USB FIFO → TinyUSB buffer → audio_task → I2S DMA → DAC
  ↓           ↓              ↓                ↓              ↓           ↓        ↓
  ?         125μs         variable         12.5KB        10-50μs      ~1ms     ~5μs
```

**Latencias del sistema:**
1. **USB Bus:** 125 μs (HS microframe interval)
2. **TinyUSB buffer:** 12.5 KB = ~4 ms @ 384kHz
3. **I2S DMA buffer:** ~1 ms
4. **audio_task:** 10-50 μs (polling actual)

**Total pipeline latency:** ~5-6 ms (dominado por buffers, no por polling)

**Conclusión:**
- Optimizar audio_task de 50μs → 1μs solo reduce latencia total de 5.05ms → 5.001ms (**mejora de 0.01%**)
- El cuello de botella son los buffers USB e I2S, no el scheduling
- **Recomendación:** Mantener polling actual (simple, funciona, bajo overhead)

#### Cuándo considerar interrupciones

**Sí usar IRQ si:**
- ✅ Necesitas sincronización exacta con I2S para DSP en tiempo real
- ✅ Quieres minimizar CPU usage en idle (batería)
- ✅ Requieres jitter <5 μs (mediciones, analysis)

**Usar polling si:**
- ✅ Latencia total <10 ms es aceptable (✅ nuestro caso)
- ✅ Simplicidad es prioritaria
- ✅ CPU 1 está dedicado (no hay contención)

**Implementación propuesta para F3 (DSP):**

Usar **IRQ I2S DMA** solo si el DSP necesita sincronización exacta:
```c
void i2s_dma_callback(i2s_event_data_t *event_data, void *user_ctx) {
    // Procesamiento DSP síncro con I2S clock
    apply_dsp_chain(event_data->dma_buf, event_data->size);
}
```

De lo contrario, mantener polling actual.

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
| F3   | EQ / DSP Pipeline         | audio_pipeline/dsp       | ✅ Completado |
| F4   | Gestion de energia        | power, sensors           | ⏸️ Pendiente  |
| F5   | Controles fisicos         | input                    | ⏸️ Pendiente  |
| F6   | Reproduccion microSD      | storage, audio_codecs    | ⏸️ Pendiente  |
| F7   | Wireless ESP32-C5         | wireless                 | ⏸️ Pendiente  |
| F8   | UI avanzada               | ui                       | ⏸️ Pendiente  |
| F9   | Polish y features avanzados| Todos                   | ⏸️ Pendiente  |

### F0 - Estructura del Proyecto ✅
- ✅ Renombrar `hello_world_main.c` -> `app_main.c`
- ✅ `project(lyra)` en CMakeLists.txt raiz
- ✅ Crear 10 directorios de componentes con placeholders
- ✅ Integración TinyUSB (sin wrapper `esp_tinyusb`)

### F0.5 - USB Audio (UAC2+CDC) ✅
**Completado:** Pipeline USB → I2S funcional

- ✅ UAC2 Speaker con feedback endpoint asíncrono
- ✅ Multi-formato: 16/24/32-bit, hasta 384kHz
- ✅ Alternate settings para cambio dinámico de formato
- ✅ CDC Serial para debug (printf sobre USB)
- ✅ Descriptores USB compuestos (IAD)
- ✅ High-Speed USB (480 Mbps, UTMI PHY)
- ✅ Arquitectura multi-core optimizada:
  - **CPU 0:** TinyUSB task, CDC task, sistema
  - **CPU 1:** audio_task dedicado (latencia mínima)
- ✅ Compatibilidad Windows/Linux/macOS

**Notas técnicas:**
- Buffer TinyUSB: 32× EP size (12.5KB) para margin
- Audio loop: `taskYIELD()` cuando no hay datos (~10-50μs latency)
- Watchdog resuelto: audio_task no interfiere con IDLE0

### F1 - I2S Output a DAC 🟡
**Estado:** Implementado temporalmente con ES8311 (placa de desarrollo)
**Objetivo final:** ES9039Q2M (placa definitiva)

**Completado (ES8311 temporal):**
- ✅ I2S configurado: 16/24/32-bit stereo con MCLK
- ✅ Sample rates: 44.1k, 48k, 88.2k, 96k, 176.4k, 192k, 384kHz
- ✅ MCLK generado: 6.144, 12.288, 24.576, 49.152 MHz
- ✅ Control GPIO: Amplifier EN (GPIO 53)
- ✅ ES8311 configurado via I2C (codec dev framework)
- ✅ Cambio dinámico de sample rate y formato
- ✅ Pipeline completo: USB → I2S → ES8311

**Pendiente (migración a ES9039Q2M):**
- ⏸️ Control SPI para registros del ES9039Q2M
- ⏸️ Control GPIO: DAC EN, Audio LDO EN
- ⏸️ Detección jack 4.4mm balanceado
- ⏸️ Configuración específica ES9039Q2M (filtros, THD+N)

#### ES9039Q2M - Capacidades Built-in (Hardware Target)

**Procesamiento interno del DAC:**

| Característica | Especificación | Control |
|----------------|----------------|---------|
| **FIR Oversampling** | 8× (4× + 2×) programable | Register 90[1:0] BYPASS_FIR |
| **Filtros digitales** | 8 presets + 1 programable | Register 88[2:0] FILTER_SHAPE |
| **IIR Filter** | Configurable, bypassable | Register 90[2] IIR_BYPASS |
| **IIR Bandwidth** | Ajustable | Register 89[2:0] IIR_BW |
| **Jitter Eliminator** | DPLL patentado (Time Domain) | Automático |
| **Volume Control** | 32-bit signed per-channel | Registros de volumen |
| **THD Compensation** | 4 coef. 16-bit (2º y 3º harm.) | THD Compensation Registers |
| **Auto Gain Cal** | Calibración chip-to-chip | Automático |
| **Mute** | Hardware mute | Control register |

**Presets de filtros (típicos familia ES903x):**
1. Brick wall
2. Corrected minimum phase fast
3. Minimum phase slow/fast
4. Linear phase slow/fast
5. Apodizing fast
6. Custom programmable

**Especificaciones:**
- **DNR:** hasta 128 dB
- **THD+N:** –120 dB típico
- **Formatos:** PCM hasta 768kHz/32-bit, DSD1024, DoP
- **Control:** SPI/I2C

**Lo que NO tiene el ES9039Q2M:**
- ❌ EQ paramétrico o gráfico
- ❌ Bass/Treble boost (tone controls)
- ❌ Crossfeed o procesamiento espacial
- ❌ Dynamic range compression/limiting

**División Hardware vs Software:**

```
┌─────────────────────────────────────────┐
│ SOFTWARE DSP (ESP32-P4 CPU 1)           │
│ • EQ Paramétrico (5-10 bandas)          │
│ • Bass/Treble Boost (shelving filters)  │
│ • Crossfeed (headphone spatialization)  │
│ • Dynamic compression (opcional)        │
│ • User presets configurables            │
└──────────────────┬──────────────────────┘
                   │ I2S
                   ↓
┌─────────────────────────────────────────┐
│ HARDWARE (ES9039Q2M)                    │
│ • Jitter Elimination (DPLL)             │
│ • Digital Filters (8 presets)           │
│ • FIR 8× Oversampling                   │
│ • IIR Filter (configurable)             │
│ • Volume Control (32-bit, sin pérdida)  │
│ • THD Compensation                      │
└─────────────────────────────────────────┘
```

**Conclusión:** El ES9039Q2M maneja el procesamiento de conversión DAC de clase audiófilo (jitter, oversampling, filtros anti-aliasing, volumen), mientras que el ESP32-P4 complementa con DSP de usuario (EQ, tone, crossfeed) que el DAC no provee.

### F2 - Display & UI Base
- Inicializacion MIPI DSI para panel 720x1280
- Integracion LVGL como framework de UI
- Driver I2C del touch panel
- UI basica: pantalla now-playing, volumen, indicador de sample rate

### F3 - EQ / DSP Pipeline ✅
**Estado:** **COMPLETADO** - DSP optimizado con FPU y budget management

**Objetivo:** Aplicar procesamiento DSP entre USB FIFO y salida I2S sin afectar latencia

**Arquitectura propuesta:**
```
USB FIFO → tud_audio_read() → [DSP Chain] → i2s_channel_write() → DAC
                                    ↓
                            (CPU 1 dedicado)
```

**Análisis de capacidad de procesamiento:**
- **CPU:** Dual RISC-V @ 400 MHz (CPU 1 dedicado para audio)
- **FPU:** Single-precision (32-bit IEEE 754) hardware
- **DSP Extensions:** RISC-V DSP instruction set (no documentado completamente)
- **Vector Extensions:** Para aceleración de cálculos NN (potencialmente útil para DSP)
- **Worst case:** 384 kHz × 2 ch × 4 bytes = 3.07 MB/s
- **Samples/sec:** 384,000 × 2 = 768,000 samples
- **Ciclos disponibles por sample:** 400,000,000 / 768,000 = **520 ciclos/sample**

**Ventajas del FPU hardware:**
- ✅ Operaciones floating-point en 1-2 ciclos (vs 10-20 en software)
- ✅ Código DSP más simple (no need para fixed-point Q31)
- ✅ Mayor precisión en cálculos de coeficientes
- ✅ Librerías optimizadas: ESP-DSP con aceleración FPU

**Tipos de filtros viables:**

#### 1. **EQ Paramétrico (Biquad IIR)** ✅ VIABLE
- **Complejidad:** ~20-30 ciclos por biquad por sample
- **Bandas soportadas:** 10-15 bandas @ 384kHz
- **Uso:** Bass, Mid, Treble, notch filters
- **Implementación:** Direct Form I o II
- **Coste:** 5-10% CPU @ 384kHz

#### 2. **EQ Gráfico (FIR)** ⚠️ LIMITADO
- **Complejidad:** N taps × 2 mults/adds por sample
- **FIR viable:** 32-64 taps @ 384kHz (respuesta limitada)
- **Uso:** Corrección de fase lineal
- **Coste:** 15-30% CPU @ 384kHz
- **Nota:** IIR es más eficiente para EQ gráfico

#### 3. **Crossfeed (Headphone Spatialization)** ✅ VIABLE
- **Complejidad:** ~4 biquads + delays + mezcla
- **Algoritmo:** Chu Moy, Jan Meier, o Bauer stereophonic
- **Uso:** Reducir fatiga con auriculares
- **Coste:** 3-5% CPU @ 384kHz

#### 4. **Dynamic Range Compression** ⚠️ COSTOSO
- **Complejidad:** RMS detection + envelope follower + gain computation
- **Uso:** Limiter, compressor, expander
- **Coste:** 10-20% CPU @ 384kHz
- **Viable:** Solo en sample rates ≤192kHz

#### 5. **Bass Boost / Treble Boost** ✅ TRIVIAL
- **Complejidad:** 1-2 biquads (shelving filters)
- **Coste:** <2% CPU @ 384kHz

#### 6. **Volume Control (software)** ✅ TRIVIAL
- **Complejidad:** 1 mult por sample
- **Coste:** <1% CPU @ 384kHz
- **Nota:** Reducir bit depth, mejor usar hardware

#### 7. **Resampling/Upsampling** ❌ NO VIABLE
- **Complejidad:** Muy alta (interpolación + anti-aliasing FIR)
- **Coste:** >80% CPU @ 384kHz
- **Nota:** No necesario (ya recibimos 384kHz de USB)

#### 8. **Room Correction (FIR largo)** ❌ NO VIABLE en tiempo real
- **Complejidad:** 2048-8192 taps (FFT convolution)
- **Coste:** >100% CPU @ 384kHz
- **Alternativa:** Pre-procesar en app companion, enviar por USB

**Propuesta de DSP Chain realista:**

```c
// CPU 1 @ 400 MHz, budget: 520 cycles/sample @ 384kHz
DSP_Chain {
    1. Volume Control          // 1 cycle/sample   (0.2%)
    2. Bass Boost (shelving)   // 20 cycles/sample (4%)
    3. Treble Boost (shelving) // 20 cycles/sample (4%)
    4. EQ Paramétrico 5-band   // 100 cycles/sample (20%)
    5. Crossfeed (opcional)    // 80 cycles/sample (15%)
    -------------------------------------------
    Total:                     // ~220 cycles/sample (42% @ 384kHz)
}
```

**Margin de seguridad:** ~300 ciclos/sample libres para:
- Cambios de formato en caliente
- Logging ocasional
- Overhead del scheduler

**Implementación:**
- ✅ Librerías optimizadas: **ESP-DSP** con aceleración FPU RISC-V
- ✅ **Floating-point arithmetic** (aprovechar FPU hardware del ESP32-P4)
- ✅ Arquitectura modular en `components/audio_pipeline/`
- ✅ Presets de EQ en NVS (Rock, Jazz, Classical, Flat, etc.)
- ✅ Bypass mode para comparación A/B
- ✅ Sistema de configuración persistente (NVS) para futura UI
- ✅ UI controls via display (F2) o app companion (F7)

**Estructura de componentes:**
```
components/audio_pipeline/
├── include/
│   ├── audio_pipeline.h      // API pública del pipeline
│   ├── dsp_chain.h           // DSP chain manager
│   ├── dsp_biquad.h          // Filtros biquad IIR (FPU)
│   ├── dsp_crossfeed.h       // Crossfeed para auriculares
│   └── dsp_presets.h         // Presets de EQ
├── audio_pipeline.c          // Integración pipeline completo
├── dsp_chain.c               // Manager de cadena DSP
├── dsp_biquad.c              // Implementación biquad
├── dsp_crossfeed.c           // Implementación crossfeed
├── dsp_presets.c             // Definición de presets
└── CMakeLists.txt
```

**Presets implementados (comandos CDC: flat, rock, jazz, classical, headphone, bass, test):**
1. **Flat:** Bypass (sin procesamiento)
2. **Rock:** Bass +12dB @ 100Hz (EXTREME para testing)
3. **Jazz:** Smooth (+2dB bass, -1dB mid @ 1kHz, +1dB treble @ 8kHz)
4. **Classical:** Natural V-shape (+3dB bass @ 120Hz, -2dB mid @ 1.5kHz, +2dB treble @ 6kHz)
5. **Headphone:** Flat + Crossfeed (TODO: implementar crossfeed)
6. **Bass Boost:** Bass +8dB @ 80Hz
7. **Test Extreme:** +20dB @ 1kHz (verificación DSP funcionando)

**Implementación completada:**
- ✅ **Biquad IIR filters** con FPU acceleration (Direct Form I)
- ✅ **Pre-calculated coefficients @ 48kHz** (instant preset switching)
- ✅ **Soft limiter (tanh)** para evitar clipping audible
- ✅ **ILP optimization** (40% speedup en biquad processing)
- ✅ **Conditional debug logging** (0% overhead en producción)
- ✅ **Budget management API** para validación dinámica de límites
- ✅ **CDC commands** para testing interactivo (help, rock, jazz, on, off, status)

**Performance verificado:**
- @ 48 kHz: 0.62% CPU (1 filtro), hasta **30 filtros** safe
- @ 384 kHz: 4.99% CPU (1 filtro), hasta **25 filtros** safe
- Preset loading: instantáneo (< 5 cycles con coeficientes pre-calculados)
- Calidad: Bit-exact, sin pérdida de fidelidad
- Soft limiting: Elimina distorsión audible con boost extremo

**Budget management:**
- Límites dinámicos según sample rate actual
- Validación antes de añadir filtros o cambiar presets
- Safety margin: 85% max CPU (15% headroom garantizado)
- API completa para integración UI (ver `DSP_BUDGET_GUIDE.md`)

**Próximos pasos (TODO):**
- Implementar crossfeed para preset Headphone
- Integrar con UI (F2) para control visual
- NVS storage para presets personalizados del usuario

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