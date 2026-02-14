# DSP Budget Management - Guía de Integración UI

## 📋 Resumen

El sistema de budget DSP permite validar dinámicamente qué puede configurar el usuario según el sample rate actual, garantizando que el sistema nunca exceda el 85% de CPU (15% de headroom de seguridad).

---

## 🎯 API Disponible

### 1. **Obtener información de budget actual**

```c
#include "dsp_chain.h"

dsp_budget_t budget;
dsp_chain_get_budget(&g_dsp_chain, &budget);

// Información disponible:
budget.cpu_freq_mhz;        // 400 MHz
budget.sample_rate;         // Sample rate actual (48000, 96000, etc.)
budget.cycles_per_sample;   // Ciclos disponibles por sample
budget.cycles_used;         // Ciclos actualmente en uso
budget.cycles_available;    // Ciclos disponibles (con safety margin)
budget.filters_active;      // Filtros actualmente activos
budget.filters_max;         // Máximo de filtros permitidos
budget.cpu_usage_percent;   // Uso de CPU actual (%)
```

### 2. **Validar antes de añadir filtros**

```c
// Ejemplo: ¿Puedo añadir 5 filtros más?
if (dsp_chain_can_add_filters(&g_dsp_chain, 5)) {
    // SÍ, hay budget suficiente
    ui_enable_add_filters_button();
} else {
    // NO, excedería el límite
    ui_disable_add_filters_button();
    ui_show_warning("CPU budget limit reached");
}
```

### 3. **Obtener límite para sample rate específico**

```c
// Útil para mostrar en UI antes de cambiar sample rate
uint8_t max_filters_48k  = dsp_chain_get_max_filters_for_rate(48000);   // 30
uint8_t max_filters_96k  = dsp_chain_get_max_filters_for_rate(96000);   // 30
uint8_t max_filters_192k = dsp_chain_get_max_filters_for_rate(192000);  // 30
uint8_t max_filters_384k = dsp_chain_get_max_filters_for_rate(384000);  // 25

// Mostrar en UI: "At 384kHz, max 25 filters allowed"
```

### 4. **Validar preset antes de cargar**

```c
// Antes de cambiar preset por usuario
if (dsp_chain_validate_preset(&g_dsp_chain, PRESET_ROCK)) {
    // SÍ, preset es válido para sample rate actual
    audio_pipeline_set_preset(PRESET_ROCK);
} else {
    // NO, preset excede límite
    ui_show_error("Preset too complex for current sample rate");
}
```

---

## 📊 Tabla de Límites por Sample Rate

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Sample Rate │ Budget   │ Max Filtros │ CPU @ 10 flt │ Presets Permitidos  │
├──────────────┼──────────┼─────────────┼──────────────┼─────────────────────┤
│   44.1 kHz   │ 9070 cyc │    30*      │    2.57%     │ Todos               │
│   48.0 kHz   │ 8333 cyc │    30*      │    2.57%     │ Todos               │
│   88.2 kHz   │ 4535 cyc │    30*      │    5.14%     │ Todos               │
│   96.0 kHz   │ 4167 cyc │    30*      │    5.14%     │ Todos               │
│  176.4 kHz   │ 2268 cyc │    30*      │   10.3%      │ Todos               │
│  192.0 kHz   │ 2083 cyc │    30*      │   10.3%      │ Todos               │
│  352.8 kHz   │ 1134 cyc │    26       │   19.4%      │ Todos (< 26 flt)    │
│  384.0 kHz   │ 1042 cyc │    25       │   20.5%      │ Todos (< 25 flt)    │
└─────────────────────────────────────────────────────────────────────────────┘

* Limitado a 30 filtros por UI (DSP_MAX_USER_FILTERS)
  Budget real permitiría más, pero 30 es suficiente para cualquier caso de uso
```

### **Presets Actuales y su Validez:**

```
Preset       | Filtros | Válido @ 48k | Válido @ 384k | CPU @ 384k
─────────────┼─────────┼──────────────┼───────────────┼────────────
Flat         |    0    |      ✅      |      ✅       |   3.26%
Rock         |    1    |      ✅      |      ✅       |   4.99%
Jazz         |    3    |      ✅      |      ✅       |   8.44%
Classical    |    3    |      ✅      |      ✅       |   8.44%
Headphone    |    0+XF |      ✅      |      ✅       |  12.85%
Bass Boost   |    1    |      ✅      |      ✅       |   4.99%
Test Extreme |    1    |      ✅      |      ✅       |   4.99%
```

---

## 🎨 Ejemplos de Integración UI

### **Ejemplo 1: Panel de configuración EQ**

```c
// Actualizar UI cuando cambia sample rate
void ui_update_eq_limits(uint32_t new_sample_rate) {
    uint8_t max_filters = dsp_chain_get_max_filters_for_rate(new_sample_rate);

    char msg[64];
    snprintf(msg, sizeof(msg), "Max filters @ %lu Hz: %d",
             new_sample_rate / 1000, max_filters);
    ui_set_status_text(msg);

    // Deshabilitar sliders si excede límite
    for (int i = max_filters; i < DSP_MAX_USER_FILTERS; i++) {
        ui_disable_filter_slider(i);
    }
}

// Validar antes de añadir filtro
void ui_on_add_filter_button_clicked() {
    if (dsp_chain_can_add_filters(&g_dsp_chain, 1)) {
        add_new_filter();
        ui_update_budget_display();
    } else {
        ui_show_alert("Cannot add filter: CPU limit reached");
    }
}
```

### **Ejemplo 2: Medidor de CPU en tiempo real**

```c
// Actualizar cada 500ms
void ui_update_cpu_meter() {
    dsp_budget_t budget;
    dsp_chain_get_budget(&g_dsp_chain, &budget);

    // Actualizar barra de progreso
    ui_set_progress_bar(budget.cpu_usage_percent);

    // Cambiar color según uso
    if (budget.cpu_usage_percent < 50.0f) {
        ui_set_meter_color(GREEN);   // Safe
    } else if (budget.cpu_usage_percent < 70.0f) {
        ui_set_meter_color(YELLOW);  // Warning
    } else {
        ui_set_meter_color(RED);     // Critical
    }

    // Mostrar info
    char info[128];
    snprintf(info, sizeof(info),
             "CPU: %.1f%% (%d/%d filters)\n"
             "Budget: %d/%d cycles\n"
             "Max: %d filters @ %lu Hz",
             budget.cpu_usage_percent,
             budget.filters_active,
             budget.filters_max,
             budget.cycles_used,
             budget.cycles_available,
             budget.filters_max,
             budget.sample_rate);
    ui_set_tooltip(info);
}
```

### **Ejemplo 3: Selector de presets con validación**

```c
// Lista de presets en UI
void ui_populate_preset_list() {
    for (eq_preset_t preset = 0; preset < PRESET_COUNT; preset++) {
        const char *name = preset_get_name(preset);
        bool valid = dsp_chain_validate_preset(&g_dsp_chain, preset);

        if (valid) {
            ui_add_preset_item(name, preset, ENABLED);
        } else {
            // Mostrar como deshabilitado con nota
            char label[64];
            snprintf(label, sizeof(label), "%s (too complex)", name);
            ui_add_preset_item(label, preset, DISABLED);
        }
    }
}

// Antes de aplicar preset seleccionado
void ui_on_preset_selected(eq_preset_t preset) {
    if (dsp_chain_validate_preset(&g_dsp_chain, preset)) {
        audio_pipeline_set_preset(preset);
        ui_show_toast("Preset loaded");
    } else {
        ui_show_error("Preset too complex for current sample rate. "
                      "Reduce sample rate or choose simpler preset.");
    }
}
```

### **Ejemplo 4: Advertencia de cambio de sample rate**

```c
// Antes de cambiar sample rate, advertir al usuario
void ui_on_sample_rate_change_requested(uint32_t new_rate) {
    uint8_t current_filters = g_dsp_chain.num_biquads;
    uint8_t max_at_new_rate = dsp_chain_get_max_filters_for_rate(new_rate);

    if (current_filters > max_at_new_rate) {
        // Mostrar diálogo de confirmación
        char warning[256];
        snprintf(warning, sizeof(warning),
                 "Changing to %lu Hz will disable %d filters.\n"
                 "Current: %d filters\n"
                 "Max @ %lu Hz: %d filters\n\n"
                 "Continue?",
                 new_rate, current_filters - max_at_new_rate,
                 current_filters, new_rate, max_at_new_rate);

        if (ui_show_confirm_dialog(warning)) {
            // Usuario acepta
            change_sample_rate(new_rate);
            // Los filtros que exceden se deshabilitarán automáticamente
        }
    } else {
        // Safe, cambiar sin warning
        change_sample_rate(new_rate);
    }
}
```

---

## ⚙️ Constantes Configurables

Si necesitas ajustar los límites, edita en `dsp_chain.h`:

```c
// Límites de hardware
#define DSP_MAX_BIQUADS 10          // Máximo en cadena (actualmente 10)

// Límites de UI
#define DSP_MAX_USER_FILTERS 30     // Máximo configurable por usuario

// Margen de seguridad
#define DSP_SAFETY_MARGIN 0.85f     // Usar 85% del budget (15% headroom)
```

Y en `dsp_chain.c`:

```c
// Costes de CPU (ajustar si cambias implementación)
#define CYCLES_BASE_OVERHEAD  34    // Conversión + limiter
#define CYCLES_PER_FILTER     18    // Biquad optimizado
#define CYCLES_CROSSFEED     100    // Crossfeed (futuro)
#define CYCLES_DRC            80    // DRC (futuro)
```

---

## 🚦 Recomendaciones de UX

### **Indicadores visuales:**

1. **Verde (< 50% CPU)**: "Safe - Add more filters"
2. **Amarillo (50-70%)**: "High usage - Monitor performance"
3. **Naranja (70-85%)**: "Critical - Avoid adding more"
4. **Rojo (> 85%)**: "Limit reached - Cannot add filters"

### **Mensajes al usuario:**

- ✅ **Éxito**: "Filter added (15 active, 45% CPU)"
- ⚠️ **Advertencia**: "Approaching CPU limit (70%)"
- ❌ **Error**: "Cannot add filter: CPU budget exceeded"

### **Tooltips informativos:**

```
"Maximum filters at current sample rate: 25
Currently using: 10 filters (40% CPU)
Available: 15 more filters"
```

---

## 📝 Notas Importantes

1. **Validación automática**: El sistema SIEMPRE valida antes de cargar presets
2. **Safety margin**: 15% headroom garantiza estabilidad
3. **Dynamic limits**: Límites se recalculan cuando cambia sample rate
4. **Backward compatible**: Presets actuales funcionan en todos los sample rates

---

## 🎯 Estado Actual

### **Presets implementados:**

- ✅ Flat (0 filtros) - Bypass
- ✅ Rock (1 filtro) - +12dB @ 100Hz
- ✅ Jazz (3 filtros) - Smooth
- ✅ Classical (3 filtros) - V-shape
- ✅ Headphone (0 filtros + crossfeed TODO)
- ✅ Bass Boost (1 filtro) - +8dB @ 80Hz
- ✅ Test Extreme (1 filtro) - +20dB @ 1kHz

### **Máximos garantizados:**

- @ 48-192 kHz: **30 filtros** (limitado por UI)
- @ 384 kHz: **25 filtros** (limitado por CPU budget)

### **Todos los presets actuales son válidos en todos los sample rates soportados (44.1-384 kHz)**
