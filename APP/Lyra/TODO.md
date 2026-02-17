# TODO - Lyra Project

> **Última actualización:** 2026-02-17
> **Estado actual:** Core de audio completo (USB + SD + DSP + I2S). Faltan capas de interacción.

---

## 📋 Estado General del Proyecto

### ✅ Fases Completadas

- **F0**: Estructura del proyecto
- **F0.5**: USB Audio (UAC2 + CDC)
- **F1**: I2S output (temporal con ES8311, objetivo ES9039Q2M)
- **F1.5**: MSC (USB Mass Storage) — **READ ~15 MB/s, WRITE ~7.2 MB/s** (double-buffer ping-pong, DMA 64B align)
- **F3**: DSP Pipeline con EQ
- **F3.1**: **Audio Pipeline decoupled architecture** — space-check, zero overflow
- **F3.2**: **Fix coeficientes biquad** — eliminado path pre-calculado con error 2x
- **F6**: Reproducción microSD — WAV/FLAC/MP3, playlist, CUE parser, audio_source manager
- **F6.1**: **I2S reconfig entre pistas SD** — same-source format change + fallback rate propagation
- **F6.2**: **SD throughput** — setvbuf 32KB, decode block 1024 frames, SD CRC safety check
- **F6.3**: **CUE sheet parser** — implementado (sin testear, falta .cue de prueba)

### 🔄 Próximas Fases

- **F2**: Display & UI base (LVGL + MIPI DSI) ← **PRÓXIMO (hardware)**
- **F3.5**: DSP Features avanzadas (EQ paramétrico 5 bandas, crossfeed, loudness)
- **F4**: Gestión de energía (MAX77972, BMA400)
- **F5**: Controles físicos (botones GPIO)
- **F7**: Wireless ESP32-C5 (BT5 + WiFi6)
- **F8**: UI avanzada
- **HW**: Migración a ES9039Q2M (placa final)

---

## 🎯 Fase F3 - DSP Pipeline (COMPLETADA)

### ✅ Lo que se implementó hoy

#### **1. Arquitectura DSP completa**
- Pipeline: `USB → DSP Chain → I2S`
- Frame-by-frame processing para latencia mínima
- In-place buffer modification (sin copias)

#### **2. Componentes implementados**

```
components/audio_pipeline/
├── include/
│   ├── audio_pipeline.h        ✅ API pública
│   ├── dsp_types.h            ✅ Tipos comunes
│   ├── dsp_chain.h            ✅ Chain manager + budget API
│   ├── dsp_biquad.h           ✅ Biquad IIR filters
│   └── dsp_presets.h          ✅ Presets + coeffs pre-calc
├── audio_pipeline.c           ✅ Integration layer
├── dsp_chain.c                ✅ DSP chain + budget mgmt
├── dsp_biquad.c               ✅ Biquad + ILP optimization
├── dsp_presets.c              ✅ 7 presets + coeffs @ 48kHz
└── CMakeLists.txt             ✅
```

#### **3. Optimizaciones aplicadas**

1. **Debug logging condicional** (`#ifdef DSP_DEBUG_LOGGING`)
   - Producción: 0% overhead
   - Debug: Logging cada 1000 frames
   - Compilar con: `idf.py -D CMAKE_C_FLAGS="-DDSP_DEBUG_LOGGING" build`

2. **Soft limiter (tanh)**
   - Threshold: 95%
   - Evita clipping audible con boost extremo
   - +10 cycles pero mejora calidad perceptual

3. **Cálculo dinámico de coeficientes**
   - Recálculo automático al cambiar sample rate
   - RBJ Audio EQ Cookbook (normalizado por a0)
   - Coste: ~230 cycles por filtro solo al cambiar preset/formato (no en hot path)

4. **ILP optimization (Instruction-Level Parallelism)**
   - Reorganización de código para FPU pipeline
   - L/R channels procesados independientemente
   - **40% speedup** en biquad processing (30 → 18 cycles)

5. **Loop unrolling + compiler hints**
   - Fast path para single-filter presets (caso común)
   - `__attribute__((hot, always_inline))`
   - `restrict` pointers para alias analysis

#### **4. Budget Management API**

Implementada API completa para validación dinámica:

```c
// Obtener info de budget actual
dsp_budget_t budget;
dsp_chain_get_budget(&dsp, &budget);

// Validar antes de añadir filtros
if (dsp_chain_can_add_filters(&dsp, 5)) {
    // OK, hay budget
}

// Límite para sample rate específico
uint8_t max = dsp_chain_get_max_filters_for_rate(384000);  // → 25

// Validar preset antes de cargar
if (dsp_chain_validate_preset(&dsp, PRESET_ROCK)) {
    load_preset(PRESET_ROCK);
}
```

Ver `DSP_BUDGET_GUIDE.md` para ejemplos completos de integración UI.

#### **5. Performance verificado**

| Sample Rate | CPU @ 1 filtro | Max Filtros Safe | Notas |
|-------------|----------------|------------------|-------|
| 48 kHz      | 0.62%          | 30*              | Limitado por UI |
| 96 kHz      | 1.15%          | 30*              | Limitado por UI |
| 192 kHz     | 2.30%          | 30*              | Limitado por UI |
| 384 kHz     | 4.99%          | 25               | Limitado por CPU |

*Hardware permite más, pero 30 es suficiente para cualquier caso de uso.

#### **6. Presets implementados**

Acceso via CDC (puerto COM/ttyUSB):
- `flat` - Bypass
- `rock` - +12dB @ 100Hz (EXTREME, para testing)
- `jazz` - 3 filtros (smooth)
- `classical` - 3 filtros (V-shape)
- `headphone` - Flat + crossfeed (TODO)
- `bass` - +8dB @ 80Hz
- `test` - +20dB @ 1kHz (verificación extrema)
- `on` / `off` - Enable/disable DSP
- `status` - Info actual

#### **7. Decisiones de diseño confirmadas**

✅ **Opción A: Bloquear y que usuario decida**
- NO auto-reducir sample rate si excede límite
- Mostrar error claro y opciones al usuario
- Dejar que usuario tome decisión informada

✅ **Límites establecidos:**
- `DSP_MAX_BIQUADS = 10` (hardware, en cadena)
- `DSP_MAX_USER_FILTERS = 30` (UI limit)
- `DSP_SAFETY_MARGIN = 0.85f` (85% CPU max, 15% headroom)

---

## 🔴 TODOs Pendientes - Fase F3.5 (DSP Features Avanzadas)

### **Prioridad ALTA**

- [ ] **EQ Paramétrico de 5 bandas (usuario configurable)**
  - 5 filtros biquad independientes controlables desde UI
  - Cada banda: frecuencia (20Hz-20kHz), ganancia (-12/+12 dB), Q (0.1-10)
  - Tipos por banda: Low Shelf, Peaking (×3), High Shelf
  - Frecuencias default: 60Hz, 230Hz, 1kHz, 3.5kHz, 12kHz
  - Recálculo dinámico de coeficientes al cambiar parámetros
  - Coste: 5 × 18 = 90 cycles (trivial incluso @ 384kHz)
  - Archivos: reutilizar `biquad_init()` existente, añadir API de control
  - UI: 5 sliders verticales con labels de frecuencia + visualización curva

- [ ] **Implementar Crossfeed** para mejorar imagen estéreo en auriculares
  - Algoritmo: Chu Moy (simple) o Jan Meier (natural)
  - Mezcla controlada de canal opuesto con delay + filtro paso bajo
  - Parámetro: intensidad (0-100%, default ~30%)
  - Coste estimado: ~100 cycles
  - Archivos: `dsp_crossfeed.h`, `dsp_crossfeed.c`
  - Integración en `dsp_chain.c` después de EQ, antes de limiter
  - UI: toggle on/off + slider de intensidad

- [ ] **Loudness Compensation (Equal Loudness)**
  - Compensación Fletcher-Munson: boost graves y agudos a volumen bajo
  - Curvas ISO 226 simplificadas (3-4 filtros)
  - Se activa automáticamente según nivel de volumen del sistema
  - Coste: ~3-4 filtros × 18 = 54-72 cycles
  - UI: toggle on/off (automático según volumen)

### **Prioridad MEDIA**

- [ ] **Balance L/R**
  - Control de balance izquierda/derecha (-100 a +100)
  - Implementación: multiplicación por factor (0.0 a 1.0) por canal
  - Coste: ~4 cycles (2 multiplicaciones)
  - UI: slider horizontal centrado

- [ ] **Selección de filtro digital DAC (ES9039Q2M)**
  - ES9039Q2M tiene 7 filtros digitales seleccionables via I2C
  - Opciones: Fast Roll-Off, Slow Roll-Off, Minimum Phase, Apodizing, Hybrid, Brick Wall, etc.
  - Cada uno con distinta respuesta de fase y ringing
  - Acceso: registro I2C del DAC (sin coste DSP, lo hace el DAC)
  - UI: selector dropdown con descripción de cada filtro
  - Requisito: driver I2C para ES9039Q2M (se implementará en migración hardware)

- [ ] **NVS Storage para presets personalizados**
  - Guardar configuración de EQ del usuario en flash
  - Cargar último preset al boot
  - Máximo 5-10 presets de usuario
  - API: `preset_save_to_nvs()`, `preset_load_from_nvs()`, `preset_delete_from_nvs()`
  - Almacenar: 5 bandas + crossfeed + loudness + balance + nombre

- [ ] **Integrar con UI (cuando F2 esté lista)**
  - Medidor de CPU en tiempo real
  - Selector de presets con validación
  - EQ paramétrico con sliders + curva de respuesta
  - Advertencia si cambio de sample rate excede límite
  - Ver ejemplos en `DSP_BUDGET_GUIDE.md`

- [ ] **Más presets predefinidos**
  - Pop, Metal, Electronic, Vocal, Acoustic, Podcast
  - Cada uno con 3-5 filtros optimizados
  - Basados en curvas de referencia de la industria

### **Prioridad BAJA (Futuro)**

- [ ] **Dynamic Range Compression (DRC)**
  - Limiter, compressor, expander
  - Solo viable @ ≤192kHz (coste alto ~80 cycles)
  - Útil para escucha nocturna / ambientes ruidosos

- [ ] **Room correction (offline)**
  - Pre-procesar en app companion
  - Enviar coefficients via CDC/WiFi
  - Cargar como preset personalizado

- [ ] **Adaptive EQ**
  - Analizar contenido en tiempo real
  - Ajustar EQ dinámicamente
  - Muy costoso, solo @ 48-96kHz

- [ ] **Limpieza código legacy**
  - Eliminar arrays `coeffs_48k` en `dsp_presets.c` (código muerto, ya no se usa)
  - Hacer diagnósticos condicionales con `#ifdef` en audio_task/feeder

---

## 🐛 Issues Conocidos

### **RESUELTO ✅**: Audio stuttering
- **Problema**: Audio entrecortado con DSP activo
- **Causa**: Buffer conversion overhead demasiado alto
- **Solución**: Frame-by-frame processing + ILP optimization
- **Estado**: ✅ Funciona correctamente

### **RESUELTO ✅**: EQ no audible
- **Problema**: Cambios de EQ no perceptibles
- **Causa**: Gain insuficiente (+6dB @ 80Hz no audible)
- **Solución**: Preset extremo (+12dB @ 100Hz, +20dB @ 1kHz test)
- **Estado**: ✅ Test preset confirma DSP funcionando

### **RESUELTO ✅**: Stream buffer overflow + data loss
- **Problema**: ovf=300-600/2s overflow, blk=750/2s partial writes en I2S
- **Causa 1**: `i2s_channel_write(..., 5)` → 5ms/10ms_per_tick = 0 ticks = non-blocking
- **Causa 2**: `xStreamBufferSend(..., 0)` non-blocking → FIFO no acumula → feedback no regula host
- **Solución**: Arquitectura space-check: audio_task verifica espacio antes de leer FIFO + feeder con retry loop + notificación entre tasks
- **Estado**: ✅ ovf=0, blk=0, FIFO 40-91% estable

### **RESUELTO ✅**: DSP ruido terrible @ 48kHz con presets
- **Problema**: Ruido extremo al aplicar Rock/Jazz/Classical @ 48kHz (bypass OK)
- **Causa**: Coeficientes pre-calculados `coeffs_48k` tenían b0/b1/b2 exactamente 2x demasiado altos (ganancia DC +37dB en vez de +12dB)
- **Solución**: Eliminado path pre-calculado, siempre usar `biquad_calculate_coeffs()` dinámico
- **Estado**: ✅ Todos los presets suenan correctamente

### **RESUELTO ✅**: I2S no reconfigura entre pistas SD
- **Problema**: Canciones suenan aceleradas al cambiar entre pistas con distinto sample rate (SD→SD)
- **Causa**: `audio_source_switch()` tenía `if (old == new_source) return;` que saltaba la reconfiguración I2S
- **Solución**: Comparar formato actual vs nuevo antes del early return, reconfigurar si difiere
- **Estado**: ✅ Funciona correctamente

### **RESUELTO ✅**: I2S fallback rate mismatch
- **Problema**: Si `i2s_output_init(352800)` falla y cae a 48kHz internamente, el pipeline se configura a 352800Hz
- **Causa**: `i2s_output_init` retornaba `void`, el caller no sabía la tasa real configurada
- **Solución**: Cambiado a `uint32_t i2s_output_init()` que retorna la tasa real. Todos los callers usan `actual_rate`
- **Estado**: ✅ Pipeline siempre sincronizado con I2S real

### **RESUELTO ✅**: mono_buf overflow con decode block 1024
- **Problema**: Overflow en codecs WAV/FLAC al decodificar mono con block size 1024 (buf era 480)
- **Solución**: `mono_buf[480]` → `mono_buf[1024]` en codec_wav.c y codec_flac.c
- **Estado**: ✅ Corregido

### **CONOCIDO ⚠️**: ES8311 no soporta >96kHz bien
- **Problema**: 192kHz FLAC suena raro en placa de desarrollo (ES8311)
- **Causa**: MCLK = 192000×256 = 49.15MHz excede límite del ES8311 (~24-25MHz max)
- **Estado**: Limitación de hardware dev board. ES9039Q2M (placa final) soporta hasta 50MHz MCLK
- **Workaround**: Limitar a 96kHz en dev board, o ignorar (placa final no tendrá este problema)

### **PENDIENTE ⚠️**: Crossfeed no implementado
- **Problema**: Preset Headphone no hace nada (solo flat)
- **Solución**: Implementar crossfeed (TODO prioridad alta)
- **Workaround**: Usar otros presets mientras tanto

### **PENDIENTE ⚠️**: CUE sheet sin testear
- **Problema**: Parser implementado pero sin archivo .cue de prueba
- **Solución**: Conseguir un CD rip (single FLAC/WAV + .cue) y probar comandos `play album.cue`, `track`, `next`, `prev`

---

## 📊 Notas Técnicas Importantes

### **Cycle Budget @ 384kHz**

```
CPU: 400 MHz / (384 kHz × 2 ch) = 1042 cycles/sample
Safety (85%):                     885 cycles/sample
Base overhead:                    -34 cycles (conversión + limiter)
Available for filters:            851 cycles

Max filters: 851 / 18 = 47 filtros teórico
Safe limit:  25-30 filtros recomendado
```

### **Costes de ciclos (medidos/estimados)**

| Operación | Ciclos | Notas |
|-----------|--------|-------|
| int32 → float (2ch) | 8 | FPU |
| Biquad (optimized) | 18 | ILP + FPU pipeline |
| Soft limiter | 14 | tanh + threshold |
| float → int32 (2ch) | 8 | FPU |
| Hard clipping | 4 | Final safety |
| **TOTAL (1 filter)** | **52** | **Base + 1 biquad** |
| Crossfeed (future) | 100 | Estimado |
| DRC (future) | 80 | Estimado |

### **Estructura de preset_config_t**

```c
typedef struct {
    const char *name;                        // Nombre
    const char *description;                 // Descripción
    uint8_t num_filters;                     // Número de filtros
    biquad_params_t filters[10];             // Params (freq, gain, Q)
    bool enable_crossfeed;                   // Crossfeed on/off
    const biquad_coeffs_t *coeffs_48k;       // Pre-calculados @ 48kHz
} preset_config_t;
```

### **Coeficientes biquad (RBJ Audio EQ Cookbook)**

Calculados dinámicamente por `biquad_calculate_coeffs()` en cada cambio de formato:
- omega = 2pi x freq / fs
- A = 10^(gain_db / 40)
- alpha = sin(omega) / (2 x Q)
- Formulas RBJ para cada tipo de filtro (lowshelf, highshelf, peaking, lowpass, highpass)
- Normalización por a0 (divide b0/b1/b2/a1/a2 entre a0)

**NOTA**: Los arrays `coeffs_48k` pre-calculados en `dsp_presets.c` tenian error 2x en b0/b1/b2.
El path pre-calculado fue eliminado. Ahora siempre se usa calculo dinamico.

### **Soft Limiter (tanh)**

```c
if (|sample| > 0.95) {
    sample = tanh(sample × 0.9) / 0.9
}
```

Ventajas:
- Compresión suave sin distorsión audible
- Threshold @ 95% previene clipping
- tanh natural compressor (curva sigmoidea)

Desventajas:
- +10 cycles vs hard clipping
- Vale la pena por calidad

---

## 🔧 Configuración Actual

### **Archivos principales modificados**

```
main/
├── app_main.c                     ← Integración DSP + CDC commands
├── CMakeLists.txt                 ← Dependency audio_pipeline

components/audio_pipeline/
├── include/*.h                    ← Headers DSP
├── *.c                            ← Implementación
└── CMakeLists.txt                 ← Component registration
```

### **Comandos útiles**

```bash
# Build normal (sin debug logging)
idf.py build

# Build con debug logging
idf.py -D CMAKE_C_FLAGS="-DDSP_DEBUG_LOGGING" build

# Flash y monitor
idf.py flash monitor

# CDC commands (desde terminal serial)
help           # Lista comandos disponibles
rock           # Cargar preset Rock
jazz           # Cargar preset Jazz
classical      # Cargar preset Classical
bass           # Cargar preset Bass Boost
test           # Cargar preset Test Extreme (+20dB @ 1kHz)
flat           # Bypass
on             # Enable DSP
off            # Disable DSP (bypass)
status         # Info actual (preset, DSP on/off)
```

### **Flags de compilación importantes**

```cmake
# CMakeLists.txt (main)
PRIV_REQUIRES audio_pipeline   # Dependency DSP

# Para debug logging (opcional)
add_compile_definitions(DSP_DEBUG_LOGGING)
```

---

## 🎯 Decisiones para UI (cuando F2 esté lista)

### **1. Medidor de CPU**

```
┌─────────────────────────────────────────┐
│  DSP CPU Usage                          │
│  ████████░░░░░░░░░░░░░░░░  40%         │
│  10 filters active (max 25 @ 384kHz)   │
└─────────────────────────────────────────┘

Color coding:
• Verde (0-50%): Safe
• Amarillo (50-70%): Monitor
• Naranja (70-85%): High
• Rojo (>85%): Critical
```

### **2. Validación de presets**

```c
// Antes de aplicar preset seleccionado por usuario
if (dsp_chain_validate_preset(&dsp, selected_preset)) {
    apply_preset(selected_preset);
} else {
    show_error("Preset too complex for current sample rate.\n"
               "Options:\n"
               "• Reduce sample rate\n"
               "• Choose simpler preset");
}
```

### **3. Advertencia cambio de sample rate**

```c
if (current_filters > max_at_new_rate) {
    show_warning("Changing to %d Hz will disable %d filters.\n"
                 "Continue?",
                 new_rate,
                 current_filters - max_at_new_rate);
}
```

Ver `DSP_BUDGET_GUIDE.md` para más ejemplos.

---

## 📚 Documentación Relacionada

- **`README.md`** - Documentación principal del proyecto
- **`DSP_BUDGET_GUIDE.md`** - Guía completa de integración UI con budget API
- **`components/audio_pipeline/include/*.h`** - API headers con documentación

---

## 🚀 Próximos Pasos Recomendados

### **Próxima sesión — DSP Features (F3.5):**

1. **EQ Paramétrico 5 bandas** (feature principal de producto)
   - Añadir API de control: `dsp_chain_set_band(band_idx, freq, gain_db, Q)`
   - Integrar recálculo dinámico de coeficientes
   - Testing con sweep de frecuencias

2. **Crossfeed** (mejora auriculares)
   - Investigar algoritmo Chu Moy o Jan Meier
   - Crear `dsp_crossfeed.c` y `dsp_crossfeed.h`
   - Integrar en `dsp_chain.c`

3. **Balance L/R** (implementación trivial)
   - Añadir a `dsp_chain_t`: `float balance_l`, `float balance_r`
   - Aplicar después de EQ, antes de limiter

### **A medio plazo — Software:**

1. **CUE sheet testing** (cuando tengas un .cue de prueba)
2. **Loudness compensation** (Fletcher-Munson)
3. **NVS Storage** para guardar presets de usuario
4. **Más presets** predefinidos (Pop, Metal, Electronic, Vocal, Acoustic)

### **Hardware — Placa final:**

1. Migrar de ES8311 a ES9039Q2M (driver SPI para registros)
2. Implementar selector de filtro digital del DAC (7 opciones)
3. Ajustar MCLK para 384kHz (49.15 MHz con MCLK×128)
4. Display MIPI DSI + LVGL (F2)
5. Botones GPIO (F5)
6. MAX77972 power management (F4)
7. SDMMC DDR50 experimental (si se necesita más throughput SD)

---

## 📊 Arquitectura Audio Pipeline (Actual)

```
USB Host ──→ TinyUSB FIFO (12.5KB) ──→ audio_task (DSP) ──┐
                ↑                                          │
                └── async feedback ←── FIFO level          │
                                                           ↓
SD Card ──→ sd_player_task (decode) ──→ audio_source ──→ StreamBuffer (16KB) ──→ i2s_feeder_task ──→ I2S DMA ──→ DAC
        WAV/FLAC/MP3 codecs              manager              ↓ notify
        setvbuf 32KB                   (USB/SD switch)    xTaskNotifyGive()
        1024 frames/block              i2s_output_init()
                                       (returns actual_rate)
```

- **audio_task** (prio 5, core 1, 12KB stack): Lee FIFO solo si hay espacio en stream buffer, aplica DSP, escribe non-blocking
- **i2s_feeder_task** (prio 4, core 1, 8KB stack): Lee stream buffer, escribe I2S con retry loop (timeout 100ms), notifica audio_task
- **sd_player_task** (prio 3, core 0, 8KB stack): Decodifica SD audio, escribe al stream buffer, detecta cambios de formato
- **audio_source manager**: Conmuta USB/SD, flush buffers, reconfig I2S con rate real (fallback-safe)
- **Resultado USB**: ovf=0, blk=0, FIFO 40-91%, loop=220us
- **Resultado SD**: WAV/FLAC/MP3 playback, auto-reconfig I2S entre pistas con distinto formato

## ✅ Checklist de Continuación

### Core Audio (COMPLETADO)
- [x] **DSP compilando sin warnings**
- [x] **Presets funcionando correctamente** (coeficientes dinámicos)
- [x] **Pipeline estable** (zero overflow, zero data loss)
- [x] **Formato switching USB** (192kHz ↔ 48kHz sin problemas)
- [x] **SD Player** — WAV, FLAC, MP3, playlist, avance pistas
- [x] **I2S reconfig entre pistas SD** (same-source + fallback rate)
- [x] **MSC optimizado** (15 MB/s read, 7.2 MB/s write)
- [x] **CUE parser** implementado (pendiente testing)
- [x] **SD throughput** — setvbuf 32KB, decode block 1024

### Pendiente Software
- [ ] **CUE sheet testing** ⏸️ (falta archivo .cue de prueba)
- [ ] **EQ Paramétrico 5 bandas** ⏸️ (siguiente feature DSP)
- [ ] **Crossfeed implementado** ⏸️ (pendiente)
- [ ] **Loudness compensation** ⏸️ (pendiente)
- [ ] **Balance L/R** ⏸️ (trivial)
- [ ] **NVS storage** ⏸️ (pendiente)
- [ ] **Limpieza código legacy** ⏸️ (coeffs_48k muertos, diagnósticos)

### Pendiente Hardware / Integración
- [ ] **Display MIPI DSI + LVGL** (F2)
- [ ] **ES9039Q2M DAC** (placa final — SPI control, I2S data)
- [ ] **Botones GPIO** (F5)
- [ ] **MAX77972 power management** (F4)
- [ ] **ESP32-C5 wireless** (F7)
- [ ] **SDMMC DDR50 experimental** (investigado, no implementado)

---

**Fin del TODO - Actualizar según progreso**
