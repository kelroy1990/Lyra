/**
 * spotify_glue.cpp — Lyra Spotify Connect integration via cspot library.
 *
 * Architecture:
 *   Zeroconf: esp_http_server on port 8080, mDNS _spotify-connect._tcp
 *   Session:  cspot LoginBlob → Context → SpircHandler → TrackPlayer
 *   Audio:    int16 PCM 44100 Hz stereo → int32 left-justified → process_audio()
 *
 * All extern "C" API functions for spotify.h are defined here.
 */

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* cspot core */
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"

/* bell utilities used internally by cspot */
#include "BellLogger.h"
#include "BellTask.h"
#include "CircularBuffer.h"

extern "C" {
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "spotify.h"
}

static const char *TAG = "spotify";

/* ---- Must match audio_source_t in main/audio_source.h ---- */
#define AUDIO_SRC_NONE 0
#define AUDIO_SRC_NET  3

/* ---- Shared state ---- */
static spotify_audio_cbs_t s_cbs     = {};
static std::string         s_devname;

static std::atomic<bool>  s_active  {false};
static std::atomic<bool>  s_playing {false};
static std::atomic<bool>  s_got_blob{false};
static std::atomic<int>   s_volume  {65535};  /* 0-65535; 65535=full */

/* ---- Forward declarations ---- */
class LyraSpotifyPlayer;

static std::shared_ptr<cspot::LoginBlob>    g_blob;
static std::shared_ptr<cspot::Context>      g_ctx;
static std::shared_ptr<cspot::SpircHandler> g_handler;
static std::shared_ptr<LyraSpotifyPlayer>   g_player;

/* ==================================================================
 * Helpers — URL-encoded body parsing (Spotify Zeroconf POST)
 * ================================================================== */

static std::string url_decode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out += (char)strtol(hex, nullptr, 16);
            i += 3;
        } else if (in[i] == '+') {
            out += ' ';
            ++i;
        } else {
            out += in[i++];
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_urlencoded(const char *body)
{
    std::map<std::string, std::string> m;
    std::string s = body;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eq  = s.find('=', pos);
        if (eq == std::string::npos) break;
        size_t amp = s.find('&', eq);
        if (amp == std::string::npos) amp = s.size();
        std::string key = url_decode(s.substr(pos, eq - pos));
        std::string val = url_decode(s.substr(eq + 1, amp - eq - 1));
        m[key] = val;
        pos = amp + 1;
    }
    return m;
}

/* ==================================================================
 * Zeroconf HTTP handlers (esp_http_server)
 * ================================================================== */

static esp_err_t zeroconf_get(httpd_req_t *req)
{
    if (!g_blob) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No blob");
        return ESP_FAIL;
    }
    std::string info = g_blob->buildZeroconfInfo();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, info.c_str(), (ssize_t)info.size());
    return ESP_OK;
}

static esp_err_t zeroconf_post(httpd_req_t *req)
{
    static const char OK_RESP[] =
        R"({"status":101,"spotifyError":0,"statusString":"ERROR-OK"})";

    if (!g_blob) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No blob");
        return ESP_FAIL;
    }

    /* Read POST body (URL-encoded form data from Spotify app) */
    int total = req->content_len;
    if (total > 0 && total < 8192) {
        std::vector<char> body(total + 1, 0);
        int got = httpd_req_recv(req, body.data(), total);
        if (got > 0) {
            body[got] = '\0';
            auto params = parse_urlencoded(body.data());
            g_blob->loadZeroconfQuery(params);
            s_got_blob = true;
            ESP_LOGI(TAG, "Zeroconf: got credentials from Spotify app");
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, OK_RESP, (ssize_t)(sizeof(OK_RESP) - 1));
    return ESP_OK;
}

/* ==================================================================
 * LyraSpotifyPlayer — consumes PCM from cspot, feeds Lyra pipeline
 * ================================================================== */

class LyraSpotifyPlayer : public bell::Task {
public:
    std::shared_ptr<cspot::SpircHandler> handler;
    std::unique_ptr<bell::CircularBuffer> circ;
    std::atomic<bool> paused{true};

    LyraSpotifyPlayer(std::shared_ptr<cspot::SpircHandler> h)
        : bell::Task("sp_player", 8 * 1024, -1, 1), handler(h)
    {
        /* 128 KB ring buffer ≈ 740 ms at 44100 Hz stereo int16 */
        circ = std::make_unique<bell::CircularBuffer>(128 * 1024);

        handler->getTrackPlayer()->setDataCallback(
            [this](uint8_t *data, size_t bytes, std::string_view) -> size_t {
                return feed(data, bytes);
            });

        handler->setEventHandler(
            [this](std::unique_ptr<cspot::SpircHandler::Event> ev) {
                on_event(std::move(ev));
            });

        startTask();
    }

    /* Feed raw int16 PCM into the ring buffer (blocking) */
    size_t feed(uint8_t *data, size_t bytes)
    {
        size_t rem = bytes;
        while (rem > 0) {
            size_t w = circ->write(data + (bytes - rem), rem);
            if (w == 0) vTaskDelay(pdMS_TO_TICKS(10));
            rem -= w;
        }
        return bytes;
    }

    void on_event(std::unique_ptr<cspot::SpircHandler::Event> ev)
    {
        using ET = cspot::SpircHandler::EventType;
        switch (ev->eventType) {
        case ET::PLAY_PAUSE:
            paused   = std::get<bool>(ev->data); /* true=paused */
            s_playing = !paused;
            break;
        case ET::FLUSH:
        case ET::SEEK:
            circ->emptyBuffer();
            break;
        case ET::PLAYBACK_START:
            circ->emptyBuffer();
            paused    = false;
            s_playing = true;
            s_active  = true;
            /* TODO(source-priority): unconditionally takes the source — see net_audio_cb_switch_source
             * in app_main.c for the full TODO on a proper priority/handoff system. */
            ESP_LOGI(TAG, "PLAYBACK_START — switching to NET source (44100 Hz 32-bit)");
            if (s_cbs.switch_source)
                s_cbs.switch_source(AUDIO_SRC_NET, 44100, 32);
            ESP_LOGI(TAG, "Playback started, source = NET");
            break;
        case ET::VOLUME: {
            int vol = std::get<int>(ev->data);  /* 0-65535 */
            s_volume.store(vol);
            ESP_LOGI(TAG, "Volume: %d/65535 (%.0f%%)", vol, vol * 100.0f / 65535.0f);
            break;
        }
        case ET::DISC:
            // DISC fires as a SPIRC race condition: the Spotify app sends a
            // kMessageTypeNotify with its own device_state.is_active=true right
            // after the Load frame, and cspot misinterprets it as "another player
            // took control".  The session remains alive — do NOT switch the audio
            // source here.  A real disconnect is caught by the session loop catch
            // block in LyraSpotifyTask, which tears down properly.
            s_active  = false;
            s_playing = false;
            ESP_LOGI(TAG, "Spotify DISC event (session likely still alive — not switching source)");
            break;
        default:
            break;
        }
    }

    void runTask() override
    {
        /* Register as audio producer so the I2S task can notify us */
        if (s_cbs.set_producer_handle)
            s_cbs.set_producer_handle(xTaskGetCurrentTaskHandle());

        /*
         * Process in 256-frame chunks (1024 bytes int16 → 2048 bytes int32).
         * Spotify streams: 44100 Hz, 16-bit, stereo.
         * We expand int16 → int32 left-justified (upper 16 bits = sample).
         */
        constexpr size_t IN_BYTES    = 1024; /* bytes of int16 per chunk    */
        constexpr size_t OUT_SAMPLES = 512;  /* int32 samples (256 L + 256 R) */

        std::vector<uint8_t>  in_buf(IN_BYTES);
        std::vector<int32_t>  out_buf(OUT_SAMPLES);

        while (true) {
            if (paused || !s_active) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            size_t got = circ->read(in_buf.data(), IN_BYTES);
            if (got == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            /* Convert int16 little-endian → int32 left-justified */
            size_t samples = got / 2;
            for (size_t i = 0; i < samples; i++) {
                int16_t s = (int16_t)((uint16_t)in_buf[i * 2] |
                                      ((uint16_t)in_buf[i * 2 + 1] << 8));
                out_buf[i] = (int32_t)s << 16;
            }

            size_t stereo_frames = samples / 2;
            if (stereo_frames == 0) continue;

            /* DSP (EQ etc.) — in-place */
            if (s_cbs.process_audio)
                s_cbs.process_audio(out_buf.data(), (uint32_t)stereo_frames);

            /* Software volume scaling (0-65535, 65535 = unity gain).
             * Integer mul-shift: (sample × vol) >> 16.  Applied after DSP so
             * EQ filters operate at full precision regardless of volume setting. */
            int vol = s_volume.load();
            if (vol < 65535) {
                for (size_t i = 0; i < samples; i++)
                    out_buf[i] = (int32_t)(((int64_t)out_buf[i] * vol) >> 16);
            }

            /* Write to StreamBuffer — consumed by i2s_feeder_task.
             * portMAX_DELAY: block until space is available instead of spinning.
             * This is the natural yield point: when the StreamBuffer is full the
             * task suspends, allowing IDLE and lower-priority tasks to run. */
            if (s_cbs.get_stream_buffer) {
                StreamBufferHandle_t sb = s_cbs.get_stream_buffer();
                if (sb) {
                    size_t byte_count = stereo_frames * 2 * sizeof(int32_t);
                    xStreamBufferSend(sb, out_buf.data(), byte_count, portMAX_DELAY);
                }
            }
        }
    }
};

/* ==================================================================
 * LyraSpotifyTask — Zeroconf + cspot session lifecycle
 * ================================================================== */

class LyraSpotifyTask : public bell::Task {
public:
    LyraSpotifyTask()
        : bell::Task("sp_task", 32 * 1024, 1, 0)
    {
        startTask();
    }

    void runTask() override
    {
        ESP_LOGI(TAG, "Starting Spotify Connect — device: %s", s_devname.c_str());

        /* Create LoginBlob (holds device identity + credentials) */
        g_blob = std::make_shared<cspot::LoginBlob>(s_devname);

        /* Start esp_http_server on port 8080 for Zeroconf */
        httpd_handle_t server = nullptr;
        httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
        cfg.server_port       = 8080;
        cfg.ctrl_port         = 32769;  /* avoid conflict with DLNA's ctrl_port (32768) */
        cfg.stack_size        = 8192;
        cfg.max_open_sockets  = 4;

        if (httpd_start(&server, &cfg) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Zeroconf HTTP server");
            return;
        }

        httpd_uri_t get_uri = {
            .uri     = "/spotify_info",
            .method  = HTTP_GET,
            .handler = zeroconf_get,
            .user_ctx = nullptr,
        };
        httpd_uri_t post_uri = {
            .uri     = "/spotify_info",
            .method  = HTTP_POST,
            .handler = zeroconf_post,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &get_uri);
        httpd_register_uri_handler(server, &post_uri);

        /* Announce via mDNS — Spotify app discovers us here */
        mdns_init();
        // Hostname must be set before mdns_service_add(), otherwise
        // the internal service lookup used by mdns_service_txt_item_set()
        // fails with ESP_ERR_INVALID_STATE.  Pass TXT records inline
        // to mdns_service_add() to avoid the separate txt_item_set() calls.
        mdns_hostname_set("lyra");
        mdns_txt_item_t txt[] = {
            {"CPath",    "/spotify_info"},
            {"Stack",    "SP"},
            {"VERSION",  "1.0"},
        };
        mdns_service_add(s_devname.c_str(), "_spotify-connect", "_tcp",
                         8080, txt, 3);

        ESP_LOGI(TAG, "Zeroconf ready — open Spotify app and select '%s'",
                 s_devname.c_str());

        /* Wait for Spotify app to POST credentials */
        while (!s_got_blob) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* Tear down HTTP server — credentials received, no longer needed */
        httpd_stop(server);
        server = nullptr;

        ESP_LOGI(TAG, "Credentials received, starting session loop...");

        /* Session loop — reconnects automatically on transient errors.
         * cspot throws std::runtime_error on connection drops; we catch,
         * tear down the session objects, and retry after a short delay.
         * The Zeroconf blob is reused across reconnects (stays valid). */
        while (true) {
            try {
                g_ctx = cspot::Context::createFromBlob(g_blob);
                g_ctx->session->connectWithRandomAp();
                auto token = g_ctx->session->authenticate(g_blob);

                if (token.empty()) {
                    ESP_LOGE(TAG, "Spotify authentication failed — will retry in 5s");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }

                ESP_LOGI(TAG, "Spotify authenticated successfully");
                g_ctx->session->startTask();

                g_handler = std::make_shared<cspot::SpircHandler>(g_ctx);
                g_handler->subscribeToMercury();

                g_player = std::make_shared<LyraSpotifyPlayer>(g_handler);

                ESP_LOGI(TAG, "Session active — waiting for playback commands");
                while (true) {
                    g_ctx->session->handlePacket();
                }

            } catch (const std::exception& e) {
                ESP_LOGW(TAG, "Session error: %s — reconnecting in 3s...", e.what());
            } catch (...) {
                ESP_LOGW(TAG, "Unknown session error — reconnecting in 3s...");
            }

            /* Tear down session objects before reconnecting */
            g_player.reset();
            g_handler.reset();
            g_ctx.reset();
            s_active  = false;
            s_playing = false;
            /* Real disconnect — release audio source */
            if (s_cbs.switch_source)
                s_cbs.switch_source(AUDIO_SRC_NONE, 0, 0);

            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
};

/* ---- Global instance ---- */
static std::unique_ptr<LyraSpotifyTask> s_task;

/* ==================================================================
 * extern "C" API  (implements spotify.h)
 * ================================================================== */
extern "C" {

esp_err_t spotify_init(const char *device_name, const spotify_audio_cbs_t *cbs)
{
    s_devname = device_name ? device_name : "Lyra";
    if (cbs) s_cbs = *cbs;

    bell::setDefaultLogger();
    ESP_LOGI(TAG, "Spotify Connect initialized — device: %s", s_devname.c_str());
    return ESP_OK;
}

esp_err_t spotify_start(void)
{
    if (!s_task) {
        s_task = std::make_unique<LyraSpotifyTask>();
    }
    return ESP_OK;
}

void spotify_stop(void)
{
    s_active  = false;
    s_playing = false;
    /* Task cannot be safely stopped mid-session — device restart required */
    ESP_LOGW(TAG, "spotify_stop() called — restart device for clean state");
}

void spotify_send_pause(void)
{
    if (g_handler) g_handler->setPause(true);
}

void spotify_send_play(void)
{
    if (g_handler) g_handler->setPause(false);
}

bool spotify_is_active(void)
{
    return s_active.load();
}

spotify_status_t spotify_get_status(void)
{
    spotify_status_t st = {};
    st.connected  = s_active.load();
    st.is_playing = s_playing.load();
    return st;
}

esp_err_t spotify_logout(void)
{
    /* NVS credential clearing — stub until credential persistence is added */
    ESP_LOGW(TAG, "spotify_logout: credential persistence not yet implemented");
    return ESP_OK;
}

void spotify_handle_cdc_command(const char *subcommand,
                                spotify_print_fn_t print_fn)
{
    while (*subcommand == ' ') subcommand++;

    if (strcmp(subcommand, "status") == 0) {
        print_fn("Spotify: %s, %s\r\n",
                 s_active  ? "connected"    : "waiting for app",
                 s_playing ? "playing"      : "paused/idle");
    } else if (strcmp(subcommand, "logout") == 0) {
        spotify_logout();
        print_fn("Spotify: credentials cleared (restart to re-pair)\r\n");
    } else if (strcmp(subcommand, "pause") == 0) {
        spotify_send_pause();
        print_fn("Spotify: pause sent\r\n");
    } else if (strcmp(subcommand, "play") == 0) {
        spotify_send_play();
        print_fn("Spotify: play sent\r\n");
    } else {
        print_fn("Spotify commands: spotify status, spotify pause, "
                 "spotify play, spotify logout\r\n");
    }
}

} /* extern "C" */
