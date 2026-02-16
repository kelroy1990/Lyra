# SDMMC UHS-I / DDR Investigation — Lyra ESP32-P4

> Documento de referencia con todas las conclusiones de la investigación sobre
> la viabilidad de activar modos UHS-I (DDR50, SDR50) en el Slot 1 (GPIO Matrix)
> del ESP32-P4 para la tarjeta SD de Lyra.

---

## 1. Configuración Actual

**Archivo:** `components/storage/sd_card.c`

| Parámetro | Valor actual |
|---|---|
| Slot SDMMC | **Slot 1** (por defecto en `SDMMC_HOST_DEFAULT()`) |
| Routing | **GPIO Matrix** (no IOMUX) |
| Bus width | **4-bit** |
| Frecuencia | `SDMMC_FREQ_HIGHSPEED` (40 MHz SDR) |
| Modo DDR | **No** (SDR) |
| LDO on-chip | **LDO_VO4** para VDD_IO_5 |

### Pinout

| Señal | GPIO | Dominio de alimentación |
|---|---|---|
| SD_D0 | GPIO 39 | VDD_IO_5 |
| SD_D1 | GPIO 40 | VDD_IO_5 |
| SD_D2 | GPIO 41 | VDD_IO_5 |
| SD_D3 | GPIO 42 | VDD_IO_5 |
| SD_CLK | GPIO 43 | VDD_IO_5 |
| SD_CMD | GPIO 44 | VDD_IO_5 |
| SD_PWR (dev) | GPIO 45 | P-MOSFET, active LOW |
| SD_CD (final) | GPIO 28 | — |

---

## 2. Arquitectura SDMMC del ESP32-P4

### Dos Slots, un controlador

El ESP32-P4 tiene **un único bloque hardware SDMMC** con dos slots:

| | Slot 0 | Slot 1 |
|---|---|---|
| Routing | **IOMUX** (pines fijos GPIO 39-48) | **GPIO Matrix** (cualquier GPIO) |
| Pines fijos | GPIO 39-48 (CLK=43, CMD=44, D0-D3=39-42, D4-D7=45-48) | Configurable |
| UHS-I (oficial) | Sí (v5.5+) | No documentado |
| Max freq oficial | 200 MHz (SDR104, v6.0) | 40 MHz (High Speed) |
| Dominio potencia | VDD_IO_5 (switchable 3.3V/1.8V) | Depende de los GPIO elegidos |

### Modos de velocidad soportados oficialmente (ESP-IDF v5.5 stable)

| Modo | Clock | Throughput teórico | Voltaje | Slot oficial |
|---|---|---|---|---|
| Default Speed | 20 MHz | ~10 MB/s | 3.3V | 0 o 1 |
| High Speed | 40 MHz | ~20 MB/s | 3.3V | 0 o 1 |
| High Speed DDR | 40 MHz DDR | ~20 MB/s | 3.3V | 0 o 1 (**solo eMMC**) |
| **UHS-I DDR50** | 50 MHz DDR | **~50 MB/s** | **1.8V** | **Solo Slot 0** |
| **UHS-I SDR50** | 100 MHz | **~50 MB/s** | **1.8V** | **Solo Slot 0** |
| UHS-I SDR104 | 200 MHz | ~100 MB/s | 1.8V | Solo Slot 0 (v6.0+, Rev3) |

---

## 3. Hallazgo Clave: No hay restricción en silicio ni en software

### 3.1 Registros hardware — soportan ambos slots

El registro UHS del controlador SDMMC (`sdmmc_struct.h`):

```c
typedef union {
    struct {
        uint32_t volt:2;   // 1 bit POR SLOT — switching 1.8V
        uint32_t reserved_0:14;
        uint32_t ddr:2;    // 1 bit POR SLOT — modo DDR
        uint32_t reserved_18:14;
    };
    uint32_t val;
} sdhost_uhs_reg_t;
```

**Ambos bits (volt y ddr) existen para Slot 0 Y Slot 1.** El silicio no impide UHS-I en Slot 1.

### 3.2 HAL — sin validación de slot

`sdmmc_ll.h` (ESP32-P4):

```c
static inline void sdmmc_ll_enable_1v8_mode(sdmmc_dev_t *hw, uint32_t slot, bool en)
{
    if (en) hw->uhs.volt |= BIT(slot);
    else    hw->uhs.volt &= ~BIT(slot);
}

static inline void sdmmc_ll_enable_ddr_mode(sdmmc_dev_t *hw, uint32_t slot, bool en)
{
    if (en) {
        hw->uhs.ddr |= BIT(slot);
        hw->emmcddr.halfstartbit_reg |= BIT(slot);
    } else {
        hw->uhs.ddr &= ~BIT(slot);
        hw->emmcddr.halfstartbit_reg &= ~BIT(slot);
    }
}
```

Acepta cualquier slot sin restricción.

### 3.3 Driver host — sin checks de slot para UHS-I

`sdmmc_host.c`:

```c
// Almacena flag UHS-I para cualquier slot, sin validar slot number
if (slot_config->flags & SDMMC_SLOT_FLAG_UHS1) {
    s_host_ctx.slot_ctx[slot].is_uhs1 = true;
}
```

```c
// CMD11 voltage switching — opera en cualquier slot
void sdmmc_host_enable_clk_cmd11(int slot, bool enable)
{
    sdmmc_ll_enable_card_clock(s_host_ctx.hal.dev, slot, enable);
    sdmmc_host_clock_update_command(slot, true);
    sdmmc_ll_enable_1v8_mode(s_host_ctx.hal.dev, slot, enable);
}
```

### 3.4 Negociación de protocolo — sin checks de slot

`sdmmc_init.c` / `sdmmc_sd.c`:

```c
// Decide UHS-I basándose en card OCR + host flag, NO en slot number
bool is_uhs1 = is_sdmem && (card->ocr & SD_OCR_S18_RA) && (card->ocr & SD_OCR_SDHC_CAP);
```

La selección de DDR50, SDR50, SDR104 tampoco comprueba el slot.

### 3.5 Conclusión

| Capa | ¿Bloquea UHS-I en Slot 1? |
|---|---|
| Registros silicio (UHS reg) | **NO** — bits para ambos slots |
| HAL (`sdmmc_ll.h`) | **NO** — acepta cualquier slot |
| Driver host (`sdmmc_host.c`) | **NO** — sin validación de slot |
| Protocolo SD (`sdmmc_sd.c`, `sdmmc_init.c`) | **NO** — sin validación de slot |
| CMD11 voltage switching | **NO** — slot-agnostic |
| **Dominio de potencia (hardware PCB)** | **Depende del diseño** |
| **Timing GPIO Matrix** | **Es el verdadero reto** |

---

## 4. Por qué nuestro diseño es especial

### 4.1 Pines en VDD_IO_5

Nuestros pines de SD (GPIO 39-44) están **TODOS en el dominio VDD_IO_5** (GPIO 39-48).
Este es el mismo dominio que usa Slot 0 por IOMUX, y es switchable 3.3V/1.8V
vía el LDO on-chip (LDO_VO4) que **ya tenemos configurado** en el código.

Esto significa que el argumento de "Slot 1 no puede hacer voltage switching" **NO aplica
a nuestro diseño**, porque coincidimos en el mismo dominio de potencia que Slot 0.

### 4.2 Layout de PCB

- Trazas de **<10mm** de longitud
- Impedancia controlada a **50 Ohm**
- Retardo de PCB estimado: ~0.06 ns/mm × 10 mm ≈ **0.6 ns** (despreciable)
- Todo el presupuesto de timing está disponible para absorber el retardo del GPIO Matrix

---

## 5. El obstáculo real: retardo del GPIO Matrix

### 5.1 Cifras conocidas

La documentación del SPI Master (ESP32 clásico) indica:
- GPIO Matrix input delay: **~25 ns** (2 ciclos APB @ 80 MHz)
- GPIO Matrix output delay: **~12.5 ns** (1 ciclo APB, estimado)

**Nota importante:** Estos valores son del ESP32 original. El ESP32-P4 es un chip más
moderno y podría tener un GPIO Matrix más rápido, pero Espressif no publica cifras
específicas para el P4.

### 5.2 Impacto en cada modo UHS-I

| Modo | Clock | Ventana de datos | GPIO Matrix delay (~25ns) | Ratio | Viabilidad |
|---|---|---|---|---|---|
| **DDR50** | 50 MHz | 10 ns/edge | ~25 ns | 2.5x | **Candidato a probar** |
| SDR50 | 100 MHz | 10 ns | ~25 ns | 2.5x | Improbable |
| SDR104 | 200 MHz | 5 ns | ~25 ns | 5x | Imposible |

### 5.3 Factores que favorecen DDR50

1. **Compensación de fase**: `SOC_SDMMC_DELAY_PHASE_NUM = 4` — el controlador puede
   ajustar el punto de muestreo para compensar retardos sistemáticos.

2. **Cancelación parcial del retardo**: El CLK sale del ESP32 con retardo de GPIO Matrix,
   pero la tarjeta SD responde sincronizada a ese CLK retardado. El retardo de salida
   y entrada son parcialmente compensados — lo que importa es el **skew** (diferencia),
   no el retardo absoluto.

3. **Layout impecable**: Con 0.6 ns de retardo de PCB, no hay contribución externa
   significativa. El GPIO Matrix es el único contribuyente.

4. **El P4 puede ser más rápido**: Si el GPIO Matrix del P4 tiene menos de 25 ns de
   retardo (posible dado que es un chip más nuevo), los márgenes mejoran.

---

## 6. Plan de Implementación: DDR50 en Slot 1

### Paso 1: Habilitar UHS-I en la configuración del slot

En `components/storage/sd_card.c`, función `storage_init()`:

```c
// ANTES (línea 135):
s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

// DESPUÉS:
s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // Se ajustará post-negociación
// Habilitar DDR (ya viene por defecto en SDMMC_HOST_DEFAULT, pero explícito):
s_host.flags |= SDMMC_HOST_FLAG_DDR;
```

### Paso 2: Añadir flag UHS-I al slot

```c
// ANTES (línea 175):
slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

// DESPUÉS:
slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#ifdef LYRA_SDMMC_UHS1_EXPERIMENTAL
slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;
#endif
```

### Paso 3: Configurar frecuencia para DDR50

```c
// Para DDR50: el clock es 50 MHz, datos en ambos flancos = ~50 MB/s
#ifdef LYRA_SDMMC_UHS1_EXPERIMENTAL
s_host.max_freq_khz = 50000;  // 50 MHz para DDR50
#else
s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz SDR
#endif
```

### Paso 4: Verificar que el LDO soporta 1.8V

El LDO on-chip (LDO_VO4) ya está configurado. La secuencia CMD11 del driver
llamará a `sd_pwr_ctrl_set_io_voltage(handle, 1800)` automáticamente durante
la negociación UHS-I. Verificar que el LDO_VO4 puede bajar a 1.8V
(consultar datasheet del P4 para rango de salida del LDO_VO4).

### Paso 5: Añadir diagnósticos para evaluar resultado

Verificar en los logs que:
- `sdmmc_card_print_info()` muestra `[DDR]` y la frecuencia correcta
- `s_card->is_ddr == true`
- `s_card->is_uhs1 == true`
- `s_card->real_freq_khz` reporta 50000

### Paso 6 (si DDR50 falla): Probar overclock SDR

Como alternativa sin cambiar a UHS-I/1.8V:

```c
// Overclock SDR — la tarjeta sigue a 3.3V, sin negociación UHS-I
s_host.max_freq_khz = 52000;  // 52 MHz SDR (fuera de spec SD, dentro de spec MMC)
// Probar también: 60000, 80000 si 52 funciona
```

---

## 7. Requisitos y dependencias

### 7.1 Versión ESP-IDF

- **Mínimo recomendado**: ESP-IDF v5.5 (stable) — tiene soporte SDR50/DDR50
  y el flag `SDMMC_SLOT_FLAG_UHS1`
- v5.5.2 aún tiene el warning "not yet supported" para Slot 0 en docs,
  pero el código del driver NO tiene restricciones de slot

### 7.2 Hardware necesario

- [x] Pines SD en dominio VDD_IO_5 (GPIO 39-44) — **CUMPLIDO**
- [x] LDO on-chip para VDD_IO_5 — **CUMPLIDO** (LDO_VO4)
- [x] Diseño soporta 1.8V — **CUMPLIDO** (confirmado por el usuario)
- [x] Layout <10mm, 50 Ohm — **CUMPLIDO**
- [ ] Tarjeta SD UHS-I (U1/U3) para pruebas — **NECESARIO**

### 7.3 Tarjeta SD para pruebas

Se necesita una tarjeta que soporte UHS-I. Buscar tarjetas con el símbolo "I"
(interfaz UHS-I) y clasificación U1 o U3:
- SanDisk Extreme / Extreme Pro
- Samsung EVO Plus / PRO Plus
- Kingston Canvas Go! Plus

La tarjeta debe negociar UHS-I cuando el host lo solicite (CMD8 + ACMD41 con S18R).

---

## 8. Riesgos y plan B

### Si DDR50 falla

| Síntoma | Causa probable | Solución |
|---|---|---|
| CMD11 timeout | LDO no baja a 1.8V | Verificar config LDO_VO4, medir VDDPST_5 con osciloscopio |
| Errores CRC post-switching | Timing del GPIO Matrix | Probar ajustar delay phases manualmente |
| Card init falla completamente | Tarjeta no soporta UHS-I | Probar con otra tarjeta U3 |
| Datos corruptos | Skew excesivo en GPIO Matrix | Reducir a SDR overclock como plan B |

### Plan B: Overclock SDR (sin UHS-I)

Si DDR50 no funciona, probar incrementar la frecuencia SDR:

1. 52 MHz SDR (alta probabilidad de funcionar)
2. 60 MHz SDR (probable con buen layout)
3. 80 MHz SDR (agresivo, depende de la tarjeta)

Esto no requiere 1.8V ni negociación UHS-I — solo cambiar `max_freq_khz`.

### Plan C: Optimización de software

Maximizar throughput en 40 MHz SDR con:
- Lecturas multi-sector más grandes
- Double buffering (ya implementado para MSC)
- DMA con alineación de 64 bytes (ya implementado)

---

## 9. Estado del soporte en ESP-IDF (timeline)

| Versión | Qué añadieron |
|---|---|
| v5.3 | Soporte inicial ESP32-P4 SDMMC |
| v5.4 | Mejoras en inicialización SDMMC |
| **v5.5** (stable) | **SDR50 + DDR50 para ESP32-P4** |
| v6.0 (beta) | SDR104 (200 MHz), Slot 0 fully supported, requiere Rev3 |

Issue relevante: [espressif/esp-idf#15276](https://github.com/espressif/esp-idf/issues/15276)
— cerrado como "Won't Do" para SDR104, pero DDR50/SDR50 se implementaron igualmente en v5.5.

---

## 10. Resumen ejecutivo

**Lo que descubrimos:**
1. El silicio y el driver del ESP32-P4 **NO bloquean UHS-I en Slot 1** — la restricción
   es una recomendación de diseño, no una limitación de hardware
2. Nuestros pines GPIO 39-44 están **en VDD_IO_5**, el mismo dominio switchable
   que usa Slot 0 — podemos hacer voltage switching a 1.8V
3. El **único obstáculo real** es el retardo del GPIO Matrix (~25ns en ESP32 clásico,
   potencialmente menos en P4)
4. **DDR50 (50 MHz DDR, ~50 MB/s)** es el candidato más realista para probar
5. El layout de la PCB (<10mm, 50Ω) maximiza las posibilidades de éxito

**Lo que hay que hacer:**
1. Verificar versión ESP-IDF >= v5.5
2. Añadir `SDMMC_SLOT_FLAG_UHS1` al slot config (detrás de un `#ifdef` experimental)
3. Configurar `max_freq_khz = 50000` para DDR50
4. Probar con una tarjeta UHS-I (U1/U3)
5. Evaluar estabilidad con lecturas/escrituras prolongadas
6. Si falla, probar overclock SDR como plan B
