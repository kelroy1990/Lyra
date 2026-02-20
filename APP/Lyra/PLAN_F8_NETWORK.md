# Plan F8 — Lyra Network Audio & Services

> **Modo**: Planificación aprobada. Implementar en orden F8-A → F8-F.
> **Fecha**: 2026-02-19
> **Estado**: Diseño completo, pendiente de implementación.

---

## 0. Log de Decisiones (contexto para implementación futura)

### 0.1 Bluetooth descartado — por qué

| Chip companion | BT soportado | Conclusión |
|---|---|---|
| ESP32-C6-MINI-1 (dev board) | BLE 5.3 solamente | **Sin Classic BT → sin A2DP Sink** |
| ESP32-C5-WROOM-1U-N8R8 (placa final) | BLE 5.4 solamente | **Sin Classic BT → sin A2DP Sink** |

**Razonamiento**:
- A2DP Sink (phone → speaker) requiere **Classic Bluetooth BR/EDR**, no BLE.
- LE Audio (ISO/BIS para streaming en BLE) requiere HW diferente y Espressif confirmó
  que **NO llegará a C5/C6** (issue espressif/esp-idf #12277 — cerrado sin solución).
- Sustituir el chip companion por un ESP32-C3/C2 (con Classic BT) implicaría rediseño
  de PCB + perder WiFi 6 del C5. No rentable.
- **Alternativa elegida**: audio por WiFi — mayor calidad (HiRes FLAC vs BT SBC/AAC),
  menor latencia de sincronización, mayor alcance, sin emparejamiento BT.

### 0.2 Tidal Connect descartado — por qué

- Tidal Connect es **protocolo propietario** de Tidal (basado en Spotify Connect internamente).
- **No existe implementación open-source** de Tidal Connect para dispositivos embedded.
- **Alternativa**: DLNA/UPnP renderer + BubbleUPnP como app controladora:
  - BubbleUPnP descarga el audio de Tidal (FLAC HiRes) y lo envía a Lyra via DLNA
  - Calidad idéntica: FLAC 24bit/192kHz — sin diferencia audible vs Tidal Connect nativo
  - Funciona también con Qobuz, Spotify, librería local, radio internet

### 0.3 Web server descartado — por qué

- El producto tiene **pantalla + botones físicos** → toda configuración desde menú del dispositivo.
- Un web server embebido añade superficie de ataque, código extra, y duplica UI.
- **Diferido**: TODO[Pantalla] — cuando se integre la pantalla, el menú ya tendrá la config de red.
- WiFi provisioning en primer arranque: se manejará desde menú de pantalla (SmartConfig o
  escaneo de redes disponibles). De momento en desarrollo se usa `wifi_config_t` hardcodeada
  en NVS o via CDC command `wifi ssid <X> pass <Y>`.

### 0.4 BLE Remote Control descartado — por qué

- Dispositivo tiene pantalla + botones físicos → no necesita control remoto BLE.
- **Diferido**: TODO[App/BLE] — si se diseña app compañera "Lyra Remote" en el futuro,
  se implementará NimBLE host + GATT server entonces. Fuera de alcance de F8.

---

## TODOs diferidos (fuera de F8)

> **TODO [Pantalla]** — WiFi provisioning + configuración de red desde menú:
> Cuando se integre la pantalla (display + botones), la config de red
> (SSID, password, fuente de audio, EQ) se hará desde el menú del dispositivo.
> Evaluar: SmartConfig (ESP-IDF), escaneo de redes con selector en pantalla,
> o QR code via pantalla → configuración desde móvil.
> No se implementa web server embebido.

> **TODO [App/BLE]** — BLE Remote Control:
> El dispositivo tiene pantalla y botones físicos, no necesita control remoto BLE.
> Si en el futuro se diseña una app móvil compañera (Lyra Remote), se implementará
> entonces NimBLE host + GATT server con perfiles: Media Control, Volume Control.
> De momento: no prioritario.

---

## 1. Arquitectura General F8

```
┌────────────────────────────────────────────────────────────────────┐
│                        LYRA ESP32-P4                               │
│                                                                    │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────────┐  │
│  │  USB Audio  │  │  SD Player  │  │   NET Audio (F8-A)        │  │
│  │  (UAC2/HS)  │  │  (FLAC/MP3) │  │  DLNA · Spotify · HTTP   │  │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬──────────────┘  │
│         └────────────────┴──────────────────────┘                 │
│                          │  audio_source_switch()                  │
│                          │  NONE / USB / SD / NET                  │
│                   ┌──────▼──────┐                                  │
│                   │ StreamBuffer │  16KB ring (FreeRTOS)           │
│                   └──────┬──────┘                                  │
│                   ┌──────▼──────┐                                  │
│                   │ DSP Pipeline │  EQ · FX · Volume               │
│                   └──────┬──────┘                                  │
│                          │ I2S APLL                                 │
│                   ┌──────▼──────┐                                  │
│                   │  ES9039Q2M  │  32-bit / 768kHz                 │
│                   └─────────────┘                                  │
│                                                                    │
│  Servicios de red (via C5 SDIO):                                   │
│  DLNA renderer · Spotify Connect · OTA P4+C5 · Metadata downloader│
└────────────────────────────────────────────────────────────────────┘
          ↕ SDIO Slot 1 GPIO Matrix, SDR50 100MHz UHS-I 1.8V
┌───────────────────────────────────────────────────────────────────┐
│   ESP32-C5-WROOM-1U-N8R8 (esp_hosted slave)  WiFi 6 + BLE 5.4   │
│   Dev board: ESP32-C6-MINI-1 — mismo SDIO, misma API             │
└───────────────────────────────────────────────────────────────────┘
```

**Fuentes de audio disponibles en F8:**

| Fuente | Protocolo | Calidad máx | Estado |
|---|---|---|---|
| USB (UAC2) | USB HS OTG | 32bit/384kHz PCM | Ya implementado |
| SD Card | dr_flac/mp3/wav | 32bit/384kHz FLAC | Ya implementado |
| DLNA renderer | HTTP directo desde servidor | FLAC 32bit/384kHz | F8-B |
| Spotify Connect | cspot → Ogg Vorbis | 320kbps | F8-E |
| URL directa HTTP | net_audio_start(url) | Según servidor | F8-A |

---

## 2. Extensión de audio_source.h/c

### 2.1 Enum (audio_source.h)

```c
// Archivo: main/audio_source.h
typedef enum {
    AUDIO_SOURCE_NONE,   // transición / silencio
    AUDIO_SOURCE_USB,    // UAC2 host → tud_audio callbacks
    AUDIO_SOURCE_SD,     // SD card → sd_player_task
    AUDIO_SOURCE_NET,    // Red → net_audio_task  ← NUEVO F8-A
} audio_source_t;
```

### 2.2 Cambios en audio_source.c

Añadir el nombre en el array de strings y extender `audio_source_switch()`:

```c
// Nombres para logging/CDC
static const char *source_names[] = {"NONE", "USB", "SD", "NET"};

// En audio_source_switch(), caso de salida de NET:
case AUDIO_SOURCE_NET:
    net_audio_pause();          // para consumo sin cerrar socket
    // notificar al servicio activo:
    if (dlna_is_active())    dlna_notify_pause();
    if (spotify_is_active()) spotify_send_pause();
    break;

// En audio_source_switch(), caso de entrada a NET:
// (ya estaba pausado, solo reanudar)
if (new_source == AUDIO_SOURCE_NET) {
    net_audio_resume();
    if (dlna_is_active())    dlna_notify_play();
    if (spotify_is_active()) spotify_send_play();
}
```

### 2.3 Comportamiento al cambiar fuente

**Salir de NET (usuario cambia a SD o USB):**
1. `net_audio_pause()` → bloquea consumo del stream, **no cierra socket HTTP**
2. Notificar al servicio activo: DLNA → `AVTransport PAUSE`; Spotify → `pause via cspot`
3. `audio_source_switch(SD/USB, sample_rate, bits)` → 50ms silencio + StreamBuffer flush

**Volver a NET:**
1. `audio_source_switch(NET, 0, 0)` → sin cambio de formato (lo gestiona net_audio)
2. `net_audio_resume()` → reanuda lectura desde socket **sin reconexión** (≈ 0 latencia)
3. Notificar: `dlna_notify_play()` / `spotify_send_play()`

**Pérdida de WiFi mientras NET está activo:**
1. `net_audio_task` detecta error de lectura del socket (errno ECONNRESET / timeout)
2. Transición interna: `NET_STATE_BUFFERING` con retry exponencial (1s, 2s, 4s, 8s)
3. Si WiFi se recupera en < 30s: reconecta automáticamente (DLNA: rerequiere URI; Spotify: cspot reconecta)
4. Si > 30s sin WiFi: `audio_source_switch(AUDIO_SOURCE_NONE, 0, 0)` + notificación CDC/display

---

## 3. Fases de Implementación

### F8-A — `components/net_audio/` — Motor HTTP streaming

**Prerrequisito de todo lo demás.** Sin este componente, F8-B y F8-E no tienen dónde inyectar audio.

```
components/net_audio/
├── include/net_audio.h      ← API pública
├── net_audio.c              ← task + state machine + backpressure
├── http_stream.c            ← esp_http_client wrapper + ICY metadata headers
└── CMakeLists.txt
```

**API pública:**
```c
// Inicializar con callbacks al sistema de audio (mismo patrón que sd_player)
void net_audio_init(const net_audio_audio_cbs_t *cbs);

// Abrir stream (URL + hint de codec para negociación)
// codec_hint: "flac", "mp3", "aac", "ogg", "wav", NULL (autodetect por Content-Type)
esp_err_t net_audio_start(const char *url, const char *codec_hint);

void net_audio_stop(void);    // cierra conexión, libera buffers
void net_audio_pause(void);   // detiene consumo, mantiene conexión abierta
void net_audio_resume(void);  // reanuda sin reconexión

typedef enum {
    NET_AUDIO_IDLE,
    NET_AUDIO_CONNECTING,
    NET_AUDIO_BUFFERING,   // llenando pre-buffer antes de comenzar decode
    NET_AUDIO_PLAYING,
    NET_AUDIO_PAUSED,
    NET_AUDIO_ERROR,
} net_audio_state_t;

net_audio_state_t net_audio_get_state(void);
```

**Callbacks al sistema de audio (mismo patrón que `sd_player_audio_cbs_t`):**
```c
typedef struct {
    int  (*get_source)(void);
    void (*switch_source)(int new_source, uint32_t sample_rate, uint8_t bits);
    void (*set_producer_handle)(TaskHandle_t handle);
    StreamBufferHandle_t (*get_stream_buffer)(void);
    void (*process_audio)(int32_t *buffer, uint32_t frames);
} net_audio_audio_cbs_t;
```

**Decoders soportados — reutilización casi total de dr_libs existentes:**

| Formato | Max calidad | Biblioteca | Estado | Nota |
|---|---|---|---|---|
| FLAC | 32bit/384kHz | `dr_flac` (ya integrado) | Adaptar read_callback | Zero código nuevo de decodificación |
| MP3 | 320kbps | `dr_mp3` (ya integrado) | Adaptar read_callback | Ídem |
| WAV/PCM | 32bit/192kHz | `dr_wav` (ya integrado) | Adaptar read_callback | Ídem |
| AAC/M4A | 320kbps | **FDK-AAC** | **Nuevo** | Preferido sobre faad2: mayor calidad, licencia más permisiva, mantenido por Fraunhofer |
| Ogg Vorbis | 320kbps | **tremor** (fixed-point) | **Nuevo** | Para Spotify vía cspot; tremor = libvorbis sin floating-point |

> **Clave de diseño — reutilización de dr_libs**:
>
> `dr_flac_open(read_cb, seek_cb, tell_cb, userdata, alloc_cbs)` ya tiene API de callbacks.
> Solo se proporcionan callbacks que leen del socket HTTP en lugar de `FILE*`.
> **Cero código de decodificación nuevo** para FLAC, MP3, WAV.
>
> ```c
> // Ejemplo: callback de lectura HTTP para dr_flac
> static size_t http_read_cb(void *userdata, void *buf, size_t n) {
>     http_stream_t *hs = (http_stream_t *)userdata;
>     return http_stream_read(hs, buf, n);  // esp_http_client_read()
> }
> // ... y el decoder se abre igual que con FILE:
> drflac *flac = drflac_open(http_read_cb, http_seek_cb, http_tell_cb, &hs, NULL);
> ```

**Pre-buffer antes de comenzar reproducción:**
- Llenar 128KB del StreamBuffer antes de iniciar decode → evita underruns en conexiones lentas
- Estado `NET_AUDIO_BUFFERING` mientras se llena; `NET_AUDIO_PLAYING` al superar umbral

**ICY Metadata (radio internet):**
- Parsear headers `icy-name`, `icy-metaint`, `StreamTitle` de la respuesta HTTP
- Propagar título de pista a CDC y futuro display

**Nota de ancho de banda:**
- FLAC 24bit/192kHz ≈ 9.2 Mbps
- WiFi 6 del C5 ≈ >50 Mbps reales → margen 5x suficiente

**⚠️ CRÍTICO — Gotchas de lwIP en ESP-IDF (CONFIG_LWIP_COMPAT_SOCKETS=0):**
```c
// INCORRECTO — getaddrinfo() → newlib stub → siempre falla:
getaddrinfo("api.musicbrainz.org", "443", &hints, &res);  // FALLA

// CORRECTO — usar prefijo lwip_ siempre:
lwip_getaddrinfo("api.musicbrainz.org", "443", &hints, &res);
lwip_freeaddrinfo(res);

// INCORRECTO — ipaddr_ntoa usa buffer estático:
printf("%s %s", ipaddr_ntoa(&a), ipaddr_ntoa(&b));  // segundo sobrescribe primero

// CORRECTO:
char buf1[16], buf2[16];
ipaddr_ntoa_r(&a, buf1, sizeof(buf1));
ipaddr_ntoa_r(&b, buf2, sizeof(buf2));
```

---

### F8-B — `components/dlna/` — Renderer UPnP/DLNA

**Desbloquea Tidal (vía BubbleUPnP), Qobuz, librería NAS, radio internet.**

**Mecanismo de funcionamiento:**
1. Lyra anuncia su presencia via **SSDP** (UDP multicast 239.255.255.250:1900)
2. App controladora (BubbleUPnP, mconnect, foobar2000) descubre Lyra en la red
3. Usuario selecciona Lyra como renderer en la app
4. App envía `SetAVTransportURI` con URL directa del audio al servidor (Tidal CDN, NAS, etc.)
5. Lyra descarga el audio directamente del servidor → `net_audio_start(url)`
6. App controla playback via `AVTransport PLAY/PAUSE/STOP/SEEK`

```
components/dlna/
├── include/dlna.h
├── ssdp.c               ← SSDP UDP multicast (discovery + advertisement M-SEARCH)
├── upnp_device.c        ← Device description XML (DeviceDescription.xml)
├── avtransport.c        ← Play / Pause / Stop / Seek / SetAVTransportURI / GetPositionInfo
├── rendering_control.c  ← Volume / Mute → codec_dev volume
├── connection_manager.c ← ProtocolInfo (formatos soportados)
├── http_server_upnp.c   ← HTTP SOAP handlers + event subscriptions (NOTIFY)
├── openhome.c           ← OpenHome Playlist/Info/Time (mejor UX con Kazoo/Linn apps)
└── CMakeLists.txt
```

**API pública:**
```c
esp_err_t dlna_renderer_start(const char *device_name);   // "Lyra"
void      dlna_renderer_stop(void);
void      dlna_register_callbacks(const dlna_control_cbs_t *cbs);
void      dlna_set_position(uint32_t current_ms, uint32_t total_ms);
void      dlna_set_transport_state(dlna_transport_state_t state);
bool      dlna_is_active(void);
void      dlna_notify_pause(void);
void      dlna_notify_play(void);
```

**Formatos anunciados en ProtocolInfo (ConnectionManager):**
```
http-get:*:audio/flac:DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000
http-get:*:audio/mpeg:*
http-get:*:audio/x-wav:*
http-get:*:audio/aac:*
http-get:*:audio/ogg:*
http-get:*:audio/x-flac:*
```

**Servicios compatibles:**

| Servicio | App controladora | Calidad | Notas |
|---|---|---|---|
| **Tidal HiRes** | BubbleUPnP (Android/iOS) | FLAC 24bit/192kHz | Requiere cuenta Tidal HiFi+ |
| **Qobuz Sublime** | BubbleUPnP / mconnect | FLAC 32bit/384kHz | Requiere cuenta Qobuz Sublime |
| **Spotify** | BubbleUPnP (bridge) | Ogg 320kbps | Alternativa a F8-E para móvil |
| **NAS local** | foobar2000, JRiver, MinimServer | Cualquier formato | DLNA server en NAS |
| **Radio Internet** | BubbleUPnP / URL CDC | MP3/AAC 320kbps | Via SetAVTransportURI |

> **NAS / HomePiNAS**: el NAS corre un servidor DLNA (MinimServer, Asset UPnP, o similar).
> BubbleUPnP (o Kazoo/Linn) navega la librería del NAS y envía URLs a Lyra como renderer.
> Lyra descarga directamente del NAS → velocidad de LAN, sin cuello de botella del teléfono.

> **Tidal Connect** (protocolo propietario, sin open-source): descartado. DLNA via BubbleUPnP
> entrega **idéntica calidad** de audio (FLAC HiRes sin recodificación) con ecosistema amplio.

**OpenHome (opcional, mejor experiencia):**
OpenHome es extensión de DLNA usada por Linn, Naim, Kazoo. Añade playlist persistente,
shuffle/repeat, artwork. Recomendado implementar si BubbleUPnP + AVTransport básico funciona.

---

### F8-C — `components/ota/` — Actualizaciones de firmware

**OTA P4 (host):**
```c
// Verifica versión en servidor, compara con running app
esp_err_t ota_check_update(const char *manifest_url, ota_version_t *out_available);

// Descarga y aplica OTA al slot inactivo. Reinicia al completar.
// progress_cb: 0-100%, puede ser NULL
esp_err_t ota_start_update(const char *firmware_url, ota_progress_cb_t progress_cb);

// Marca firmware actual como válido (llamar al inicio del sistema, tras verificación)
void      ota_mark_valid(void);

// Revierte al slot anterior en próximo arranque
esp_err_t ota_rollback(void);

// Obtiene versión actual del firmware P4 (del ota_data)
esp_err_t ota_get_running_version(char *buf, size_t len);
```

- `esp_https_ota()` nativo de ESP-IDF — validación SHA256 + TLS
- **Rollback automático**: si el nuevo FW no llama `esp_ota_mark_app_valid_cancel_rollback()`
  en los primeros 30s de boot, ESP-IDF revierte automáticamente al slot anterior
- Iniciado desde menú de pantalla (TODO[Pantalla]) o comando CDC `ota update`

**OTA C5 (companion):**
```c
esp_err_t ota_c5_check_update(const char *manifest_url, ota_version_t *out);
esp_err_t ota_c5_start_update(const char *firmware_url, ota_progress_cb_t progress_cb);
```
- `esp_hosted_ota` API (disponible en `managed_components/espressif__esp_hosted`)
- P4 descarga el binario del C5 a PSRAM, lo transmite al C5 via SDIO esp_hosted
- C5 aplica OTA internamente y reinicia; P4 detecta reconexión SDIO

**Nueva tabla de particiones** — `partitions_lyra.csv` (flash 8 MB W25Q64JVZPIQ):
```csv
# Name,   Type,  SubType,  Offset,    Size,      Flags
nvs,      data,  nvs,      0x9000,    0x6000,
otadata,  data,  ota,      0xF000,    0x2000,
phy_init, data,  phy,      0x11000,   0x1000,
ota_0,    app,   ota_0,    0x20000,   0x2C0000   # 2.75 MB slot A (activo)
ota_1,    app,   ota_1,    0x2E0000,  0x2C0000   # 2.75 MB slot B (standby)
lyra_cfg, data,  nvs,      0x5A0000,  0x40000    # config usuario persistente (WiFi, EQ, etc.)
```
> Para dev board (flash 32 MB GD25Q256EYIGR): slots más amplios posibles.
> Mantener misma estructura lógica para portabilidad de config entre dev y producción.

**Manifest JSON en servidor OTA:**
```json
{
  "p4": {
    "version": "1.1.0",
    "url": "https://updates.lyra-player.io/firmware/lyra-p4-1.1.0.bin",
    "sha256": "a3b4c5d6…",
    "min_p4_hw": "1.0",
    "changelog": "Fix SDR50 tuning, add DLNA renderer"
  },
  "c5": {
    "version": "0.3.0",
    "url": "https://updates.lyra-player.io/firmware/lyra-c5-0.3.0.bin",
    "sha256": "e7f8a9b0…",
    "changelog": "Update esp_hosted slave"
  }
}
```

**Comandos CDC:**
```
ota version          → versiones actuales P4 + C5 (+ slot activo)
ota check            → compara versiones actuales vs manifest remoto
ota update           → descarga y aplica OTA P4 (pide confirmación)
ota c5 update        → descarga OTA C5 y lo envía via SDIO
ota rollback         → programa rollback al arranque (reinicia)
```

---

### F8-D — `components/metadata/` — Metadatos y carátulas

#### D.1 Lectura local (de tags embebidos en el archivo)

| Formato | Sistema de tags | Implementación |
|---|---|---|
| MP3 | ID3v2.3 / ID3v2.4 | Parser propio (~200 líneas, solo campos usados) |
| FLAC | VORBISCOMMENT | `drflac_get_tags()` — accesible via dr_flac existente |
| WAV | INFO chunk + ID3v2 | dr_wav + parser propio para INFO RIFF |
| Ogg Vorbis | VORBISCOMMENT | Parser propio (misma lógica que FLAC) |
| AAC/M4A | iTunes atoms (©nam, ©ART, …) | Parser propio de átomos MP4 |

**Estructura interna de metadatos:**
```c
typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char album_artist[256];
    char date[32];               // "2024" o "2024-03-15"
    char genre[64];
    uint32_t track_number;
    uint32_t disc_number;
    uint32_t total_tracks;
    uint32_t total_discs;
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
    uint32_t bitrate_kbps;
    char     musicbrainz_release_id[64];    // MBID del release (si en tags)
    char     musicbrainz_track_id[64];      // MBID del recording
    char     isrc[16];                      // International Standard Recording Code
    bool     has_embedded_cover;
} lyra_track_meta_t;
```

#### D.2 Bases de datos públicas para descarga

| BD | Para qué | API endpoint | Límite | API Key |
|---|---|---|---|---|
| **MusicBrainz** | Metadata completa (artista, álbum, año, género, MBID) | `musicbrainz.org/ws/2/` REST JSON | **1 req/s** | No |
| **Cover Art Archive** | Carátulas de álbum (linked a MBID de MusicBrainz) | `coverartarchive.org/release/{mbid}` | Sin límite explícito | No |
| **Last.fm** | Carátulas fallback + bio de artista | `ws.audioscrobbler.com/2.0/` REST | 5 req/s | Sí (gratuita) |
| **AcoustID** | Fingerprinting de audio (identifica sin tags) | `api.acoustid.org/v2/lookup` | 3 req/s | Sí (gratuita) |

> **API keys** de Last.fm y AcoustID: gratuitas, se obtienen registrando la app.
> Guardadas en `lyra_cfg` partition (NVS). Usuario las introduce desde menú de pantalla
> (o CDC command `meta setkey lastfm <key>`).

#### D.3 Flujo de descarga de metadata para una pista

```
1. Leer tags existentes del archivo (ID3v2 / VORBISCOMMENT / átomos MP4)
   → si está completa y ya existe cover.jpg: saltar descarga (ya actualizado)

2. Si tiene MusicBrainz Release ID en tags → búsqueda directa por MBID (más precisa)
   Si no → búsqueda por "artist + album + title" en MusicBrainz REST:
   GET /ws/2/recording?query=artist:{A}+release:{B}+recording:{T}&fmt=json

3. Elegir mejor match (score MusicBrainz ≥ 90%; si ninguno supera → AcoustID fallback)

4. Descargar JSON de metadata del release y del recording
   → rellenar campos vacíos (no sobreescribir tags existentes con datos peores)

5. Buscar carátula en Cover Art Archive (MBID del release):
   GET coverartarchive.org/release/{mbid} → array de imágenes, seleccionar "Front"
   Si no hay imagen → buscar en Last.fm:
   album.getInfo?artist={A}&album={B}&api_key={K}&format=json → image[3] (extralarge)

6. Descargar JPEG de la carátula (máx resolución disponible, hasta ~1200x1200)

7. Guardar en SD (ver §D.4)
```

**AcoustID fallback (cuando no hay tags en absoluto):**
```
1. Calcular chromaprint del audio — primeros 120 segundos del archivo
   (chromaprint genera fingerprint de ~800 bytes a partir del PCM decodificado)
2. Enviar fingerprint a AcoustID API:
   POST api.acoustid.org/v2/lookup?client={K}&fingerprint={FP}&duration={D}
3. Recibir MBID del recording → continuar desde paso 2 del flujo anterior
```

#### D.4 Almacenamiento en SD — convención de ubicación

**Regla fundamental**: los archivos de metadata se guardan **en el mismo directorio que la música** que describen. Nunca en un directorio global separado.

```
/sdcard/Music/Pink Floyd/The Wall/
├── 01 - In The Flesh.flac
├── 02 - The Thin Ice.flac
├── 03 - Another Brick In The Wall Pt1.flac
├── ...
├── cover.jpg              ← carátula del álbum (descargada, 500-1200px)
├── cover_small.jpg        ← thumbnail 200x200 (generado localmente, para display)
└── .meta/
    ├── album.json         ← metadata del álbum (MusicBrainz release)
    ├── 01 - In The Flesh.json      ← metadata por pista (recording)
    ├── 02 - The Thin Ice.json
    └── ...
```

> **Convención `cover.jpg`**: nombre universal reconocido por Kodi, foobar2000,
> Plex, JRiver, Windows Media Player, etc. Si ya existe y es válida (JPEG ≥ 100x100),
> **no se sobreescribe** salvo que el usuario fuerce (`meta download --force`).

> **Directorio `.meta/`**: nombre con punto → oculto en Linux/macOS, no visible
> en exploradores básicos. Evita contaminar el directorio de música visualmente.

**Formato `album.json`:**
```json
{
  "mb_release_id": "b84ee12a-09ef-421b-82de-0441a926375b",
  "mb_release_group_id": "83c3a53c-b2c4-4a1f-ae58-9e4fd84b9543",
  "album": "The Wall",
  "artist": "Pink Floyd",
  "album_artist": "Pink Floyd",
  "date": "1979-11-30",
  "genre": "Progressive Rock",
  "total_tracks": 26,
  "total_discs": 2,
  "label": "Harvest Records",
  "country": "GB",
  "barcode": "5099902988726",
  "cover_url_original": "https://coverartarchive.org/release/b84ee12a/…-500.jpg",
  "cover_source": "coverartarchive",
  "downloaded_at": "2026-02-19T14:30:00Z",
  "lyra_meta_version": 1
}
```

**Formato `<track>.json`:**
```json
{
  "mb_recording_id": "3d4e5f6a-…",
  "title": "In The Flesh?",
  "artist": "Pink Floyd",
  "track_number": 1,
  "disc_number": 1,
  "duration_ms": 204000,
  "isrc": "GBAYE7900101",
  "acoustid": "7f8a9b0c-…"
}
```

#### D.5 API pública del componente

```c
// Leer metadata de un archivo (solo tags locales, sin red)
esp_err_t meta_read_local(const char *path, lyra_track_meta_t *out);

// Comprobar si el directorio ya tiene metadata descargada
bool meta_has_downloaded(const char *dir_path);

// Descargar metadata para el álbum de un archivo (usa su directorio)
// Bloquea hasta completar. Llama progress_cb(0-100).
esp_err_t meta_download_album(const char *file_or_dir_path,
                               meta_progress_cb_t progress_cb);

// Descargar metadata para una pista individual (solo .json, no cover)
esp_err_t meta_download_track(const char *file_path,
                               meta_progress_cb_t progress_cb);

// Descargar metadata recursivamente desde directorio raíz
// (para "descarga toda la librería de golpe" — usa throttling interno)
esp_err_t meta_download_library(const char *root_path,
                                 meta_progress_cb_t progress_cb);

// Obtener ruta de cover art para un directorio (busca cover.jpg)
// Devuelve ruta si existe, NULL si no hay
const char *meta_get_cover_path(const char *dir_path);

// Mostrar info completa de un archivo vía print_fn (para CDC/display)
void meta_print_track_info(const char *path, meta_print_fn_t print_fn);

// Configurar API keys (se persisten en NVS lyra_cfg)
esp_err_t meta_set_api_key(meta_service_t service, const char *key);

// Progreso de descarga en curso (0-100, o 0 si no hay descarga activa)
uint8_t meta_get_download_progress(void);

// Cancelar descarga en curso
void meta_cancel_download(void);
```

#### D.6 Comandos CDC

```
meta info <path>                → tags locales del archivo (sin red)
meta info-net <path>            → busca en MusicBrainz, muestra resultado (sin guardar)
meta download <path>            → descarga metadata del álbum de <path>
meta download-album <dir>       → descarga metadata del directorio dado
meta download-all               → descarga toda /sdcard/Music (con throttling 1 req/s)
meta download --force <path>    → descarga aunque ya exista cover.jpg
meta cover <path>               → ruta de cover.jpg del directorio de <path>
meta status                     → progreso descarga en curso (%, pista actual)
meta cancel                     → cancela descarga en curso
meta setkey lastfm <key>        → guarda API key de Last.fm en NVS
meta setkey acoustid <key>      → guarda API key de AcoustID en NVS
```

> **Nota para display**: cuando se integre la pantalla, estos mismos comandos
> se invocarán desde el menú del dispositivo. La API del componente está diseñada
> para ser usada desde cualquier contexto (CDC, display task, net_audio task).

#### D.7 Gestión de rate limiting

| Servicio | Límite | Delay mínimo entre req | Retry en 503/429 |
|---|---|---|---|
| MusicBrainz | 1 req/s | **1100 ms** | exp: 2s, 4s, 8s |
| Cover Art Archive | Sin límite | 200 ms | 1s, 2s |
| Last.fm | 5 req/s | 200 ms | 1s, 2s |
| AcoustID | 3 req/s | 350 ms | 1s, 2s |

- La descarga de una librería grande corre en background task (CPU0, prioridad baja)
- Progreso reportado via callback (0-100%) → CDC + futuro display
- Se puede cancelar en cualquier momento sin corrupción (archivo parcial → se borra)
- User-Agent: `Lyra-Player/1.0 (github.com/lyra-player/lyra)` (MusicBrainz lo requiere)

---

### F8-E — `components/spotify/` — Spotify Connect via cspot

**Biblioteca**: [cspot](https://github.com/feelfreelinux/cspot) — C++17, licencia MIT, port ESP32 activo con soporte IDF 5.x.

**Mecanismo de autenticación:**
1. Lyra anuncia servicio via **ZeroConf** (mDNS, `_spotify-connect._tcp`)
2. App Spotify en el móvil descubre "Lyra" en la red local
3. Usuario selecciona Lyra como dispositivo de reproducción
4. cspot maneja OAuth device flow internamente → tokens en NVS `lyra_cfg`
5. Audio: Ogg Vorbis 320kbps descargado por cspot directamente desde CDN de Spotify

```
components/spotify/
├── include/spotify.h
├── spotify_glue.c       ← Binding cspot ↔ ESP-IDF (heap PSRAM, lwIP sockets, tasks)
├── spotify_audio.c      ← Sink de audio: Vorbis → StreamBuffer
├── vorbis/              ← tremor decoder (fixed-point, sin libvorbis float)
│   └── [tremor sources - del repo tremor o de xiph.org]
├── vendor/cspot/        ← submódulo git (feelfreelinux/cspot)
└── CMakeLists.txt
```

**API pública:**
```c
esp_err_t spotify_start(const char *device_name);   // "Lyra" → anuncia via ZeroConf
void      spotify_stop(void);
void      spotify_send_pause(void);
void      spotify_send_play(void);
bool      spotify_is_active(void);
spotify_status_t spotify_get_status(void);          // track, artist, album, progress
```

**Requisitos:**
- Cuenta **Spotify Premium** (requerida por Spotify para Connect de terceros)
- cspot usa ~512 KB PSRAM para buffers de Vorbis y contexto de sesión
- Audio: Ogg Vorbis 320kbps → tremor → int16 stereo 44.1kHz → StreamBuffer
- Las credenciales OAuth se almacenan en NVS `lyra_cfg` (persistentes entre reinicios)

**Comandos CDC:**
```
spotify status     → estado conexión + track actual + artista
spotify logout     → borrar credenciales de NVS (fuerza reautenticación)
```

---

### F8-F (opcional) — Qobuz API directa + Last.fm scrobbling

**Qobuz API directa:**
- API REST pública para desarrolladores (developer program, API key gratuita).
- Permite browsear catálogo Qobuz y obtener **URLs de streaming directas** sin app intermediaria.
- Calidad hasta 32bit/192kHz FLAC (según cuenta del usuario).
- Requiere integración con menú de pantalla para login/browse → diferir a fase pantalla.
- Implementar solo si BubbleUPnP+DLNA no resulta suficiente para UX de Qobuz.

**Last.fm scrobbling:**
- Petición HTTP POST simple a `ws.audioscrobbler.com/2.0/?method=track.scrobble`
- Registra cada pista reproducida (SD, DLNA, Spotify) → historial en Last.fm del usuario
- Requiere API key gratuita (ya usada para metadata en F8-D)
- Implementar en `components/metadata/scrobbler.c` — misma task que el downloader

---

## 4. Presupuesto de Recursos (ESP32-P4, 32 MB PSRAM)

| Componente | PSRAM estimada | RAM interna est. | Nota |
|---|---|---|---|
| StreamBuffer (existente) | 16 KB | — | FreeRTOS heap |
| HTTP receive buffer + decode | 512 KB | ~8 KB | net_audio pre-buffer |
| DLNA XML + SOAP buffers | 64 KB | ~4 KB | Peticiones HTTP UPnP |
| cspot (Spotify Connect) | 512 KB | ~16 KB | Sesión + Vorbis |
| Metadata downloader + cache | 256 KB | ~4 KB | JSON + chromaprint |
| Cover art buffer JPEG raw | 512 KB | — | Decode JPEG para display |
| OTA download buffer | 256 KB | — | Durante actualización |
| **Total F8 adicional** | **~2.1 MB** de 32 MB | **~32 KB** | Amplio margen |

> 32 MB PSRAM disponible en dev board (y 8 MB reservados suficientes para producción con C5).
> El cuello de botella real no es RAM sino CPU: cspot (C++17) puede ser intensivo en P4@400MHz.

---

## 5. Máquina de estados — cambio de fuente de audio

```
               ┌──────────────────────────────────────────────────────┐
               │  SALIR de NET (usuario cambia a SD o USB):           │
               │  1. net_audio_pause()   → detiene lectura de socket  │
               │  2. dlna_notify_pause() / spotify_send_pause()       │
               │  3. audio_source_switch(SD/USB, rate, bits)          │
               │     → 50ms silencio + StreamBuffer flush             │
               │  4. Conexión HTTP permanece abierta (0 reconexión)   │
               │                                                       │
               │  VOLVER a NET:                                        │
               │  1. audio_source_switch(NET, 0, 0)                   │
               │  2. net_audio_resume()  → reanuda sin reconexión     │
               │  3. dlna_notify_play() / spotify_send_play()         │
               │                                                       │
               │  PÉRDIDA DE WiFi mientras NET activo:                │
               │  1. net_audio_task detecta error socket              │
               │  2. Estado: NET_AUDIO_BUFFERING + retry exponencial  │
               │     (1s, 2s, 4s, 8s — máx 30s total)               │
               │  3. Si WiFi vuelve < 30s: reconecta auto             │
               │  4. Si > 30s: audio_source_switch(NONE) + notif CDC │
               └──────────────────────────────────────────────────────┘
```

---

## 6. Resumen de Comandos CDC F8

```
# NET Audio
net status              → estado streaming actual (URL, codec, bitrate, fuente, buffer%)
net play <url>          → inicia stream directo desde URL (test / radio manual)
net stop                → para stream actual

# DLNA
dlna status             → renderer state + controlador UPnP activo + track actual
dlna pause              → enviar PAUSE al controlador UPnP
dlna resume             → enviar PLAY al controlador UPnP

# OTA
ota version             → versiones firmware P4 + C5 (slot activo)
ota check               → compara con manifest remoto
ota update              → descarga y aplica OTA P4 (pide confirmación)
ota c5 update           → descarga OTA C5 y transmite via SDIO
ota rollback            → programa rollback al próximo arranque

# Metadata
meta info <path>                → tags locales del archivo (sin red)
meta info-net <path>            → previsualiza resultado MusicBrainz (sin guardar)
meta download <path>            → descarga metadata del álbum de <path>
meta download-album <dir>       → descarga metadata del directorio
meta download-all               → descarga toda /sdcard/Music (throttling 1 req/s)
meta download --force <path>    → descarga aunque ya exista cover.jpg
meta cover <path>               → ruta de cover.jpg del directorio
meta status                     → progreso descarga en curso
meta cancel                     → cancela descarga
meta setkey lastfm <key>        → API key Last.fm → NVS
meta setkey acoustid <key>      → API key AcoustID → NVS

# Spotify Connect
spotify status          → estado conexión + track + artista + progreso
spotify logout          → borrar credenciales OAuth de NVS

# WiFi (provisional, hasta pantalla)
wifi ssid <S> pass <P>  → configurar SSID/password y reconectar
wifi status             → SSID, IP, RSSI, canal
```

---

## 7. Orden de implementación

```
F8-A  net_audio + AUDIO_SOURCE_NET      ← prerrequisito de F8-B y F8-E
F8-B  DLNA renderer                     ← desbloquea Tidal/Qobuz/NAS (máximo valor)
F8-C  OTA P4 + C5 + tabla particiones   ← crítico para producto desplegable en campo
F8-D  Metadata + descarga online        ← alta valor, independiente de F8-A/B
F8-E  Spotify Connect (cspot)           ← complejidad alta C++17, requiere F8-A
F8-F  Qobuz API directa + Last.fm       ← opcional, último
```

**Estimación de complejidad:**

| Fase | Complejidad | Dependencias externas |
|---|---|---|
| F8-A | Media | esp_http_client, dr_libs (ya presentes), FDK-AAC, tremor |
| F8-B | Alta | Protocolo SSDP/UPnP/SOAP, HTTP server, OpenHome |
| F8-C | Baja-Media | esp_https_ota (IDF nativo), esp_hosted_ota |
| F8-D | Media | lwIP sockets, cJSON, MusicBrainz REST, chromaprint |
| F8-E | Alta | cspot (C++17 en IDF C), tremor, ZeroConf/mDNS |
| F8-F | Baja | Qobuz REST API, Last.fm scrobble API |

---

## 8. Archivos a Modificar

| Archivo | Cambio | Fase |
|---|---|---|
| [main/audio_source.h](main/audio_source.h) | Añadir `AUDIO_SOURCE_NET` al enum | F8-A |
| [main/audio_source.c](main/audio_source.c) | names[], lógica pause/resume NET, WiFi-loss handler | F8-A |
| [main/app_main.c](main/app_main.c) | Llamar inits nuevos (ver §8.1) | F8-A..E |
| [main/idf_component.yml](main/idf_component.yml) | Añadir FDK-AAC, tremor como managed components | F8-A |
| [main/CMakeLists.txt](main/CMakeLists.txt) | PRIV_REQUIRES: net_audio, dlna, ota, metadata, spotify | F8-A |
| `partitions_lyra.csv` | Nueva — 2 slots OTA + lyra_cfg (ver §F8-C) | F8-C |
| `sdkconfig` | OTA enable, mDNS enable, HTTPS client, MDNS_MAX_SERVICES | F8-B/C |
| [components/wireless/wireless.c](components/wireless/wireless.c) | mDNS init en `wireless_start()` (DLNA SSDP + Spotify ZeroConf) | F8-B/E |

### 8.1 Secuencia de init en app_main.c

```c
// Al final de app_main(), después de todos los inits existentes:

// F8-C: OTA (antes que WiFi para marcar app como válida)
ota_mark_valid();

// F8-D: Metadata (no depende de WiFi activo)
metadata_init();

// Esperar WiFi conectado (wireless_wait_connected() existente)
wireless_wait_connected(portMAX_DELAY);

// F8-A: Motor HTTP streaming
net_audio_init(&net_audio_audio_cbs);

// F8-B: DLNA renderer (requiere WiFi + net_audio)
dlna_renderer_start("Lyra");

// F8-E: Spotify Connect (requiere WiFi + net_audio)
spotify_start("Lyra");
```

---

## 9. Nuevos Componentes

```
components/net_audio/    F8-A — Motor HTTP + decoders callback
components/dlna/         F8-B — UPnP/DLNA Renderer + OpenHome
components/ota/          F8-C — OTA P4 + C5, check manifest
components/metadata/     F8-D — Tags locales + MusicBrainz + Cover Art + AcoustID
components/spotify/      F8-E — cspot binding + tremor + ZeroConf
```

**Dependencias entre componentes:**
```
wireless  ←── net_audio  ←── dlna
                         ←── spotify
          ←── metadata (solo para descarga online)
          ←── ota
storage   ←── metadata (guarda en SD)
audio_codecs ←── net_audio (reutiliza dr_libs con callbacks HTTP)
```
