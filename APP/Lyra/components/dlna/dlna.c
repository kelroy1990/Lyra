#include "dlna.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dlna";

//--------------------------------------------------------------------+
// Constants
//--------------------------------------------------------------------+

#define SSDP_MCAST_ADDR     "239.255.255.250"
#define SSDP_PORT           1900
#define DLNA_HTTP_PORT      7070
#define SSDP_ALIVE_INTERVAL_S 60

//--------------------------------------------------------------------+
// Device UUIDs and type strings
//--------------------------------------------------------------------+

// Fixed UUID — in production, derive from MAC address
static const char *DEVICE_UUID = "uuid:lyra-p4-esp32-upnp-renderer-1";

static const char *DEVICE_TYPE        = "urn:schemas-upnp-org:device:MediaRenderer:1";
static const char *AV_TRANSPORT_TYPE  = "urn:schemas-upnp-org:service:AVTransport:1";
static const char *RENDER_CTRL_TYPE   = "urn:schemas-upnp-org:service:RenderingControl:1";
static const char *CONN_MGR_TYPE      = "urn:schemas-upnp-org:service:ConnectionManager:1";

//--------------------------------------------------------------------+
// Internal state
//--------------------------------------------------------------------+

typedef struct {
    bool running;
    char device_name[64];
    char local_ip[16];

    dlna_control_cbs_t cbs;

    dlna_transport_state_t transport_state;
    uint32_t current_ms;
    uint32_t total_ms;
    uint8_t  volume;
    bool     mute;
    bool     controller_connected;

    char current_uri[512];
    char current_metadata[1024];

    // HTTP server handle
    httpd_handle_t http_server;

    // SSDP socket
    int ssdp_sock;
    TaskHandle_t ssdp_task_handle;
} dlna_state_t;

static dlna_state_t s_dlna = {0};

//--------------------------------------------------------------------+
// Transport state string
//--------------------------------------------------------------------+

static const char *transport_state_str(dlna_transport_state_t st)
{
    switch (st) {
        case DLNA_TRANSPORT_STOPPED:      return "STOPPED";
        case DLNA_TRANSPORT_PLAYING:      return "PLAYING";
        case DLNA_TRANSPORT_PAUSED:       return "PAUSED_PLAYBACK";
        case DLNA_TRANSPORT_TRANSITIONING: return "TRANSITIONING";
        case DLNA_TRANSPORT_NO_MEDIA:     return "NO_MEDIA_PRESENT";
        default:                          return "STOPPED";
    }
}

//--------------------------------------------------------------------+
// Device Description XML
//--------------------------------------------------------------------+

static esp_err_t handle_device_description(httpd_req_t *req)
{
    char body[4096];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
          "<specVersion><major>1</major><minor>0</minor></specVersion>"
          "<device>"
            "<deviceType>%s</deviceType>"
            "<friendlyName>%s</friendlyName>"
            "<manufacturer>Lyra Project</manufacturer>"
            "<manufacturerURL>https://github.com/lyra-player/lyra</manufacturerURL>"
            "<modelDescription>Lyra ESP32-P4 Audio Renderer</modelDescription>"
            "<modelName>Lyra</modelName>"
            "<modelNumber>1.0</modelNumber>"
            "<UDN>%s</UDN>"
            "<serviceList>"
              "<service>"
                "<serviceType>%s</serviceType>"
                "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
                "<SCPDURL>/AVTransport.xml</SCPDURL>"
                "<controlURL>/AVTransport/control</controlURL>"
                "<eventSubURL>/AVTransport/event</eventSubURL>"
              "</service>"
              "<service>"
                "<serviceType>%s</serviceType>"
                "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
                "<SCPDURL>/RenderingControl.xml</SCPDURL>"
                "<controlURL>/RenderingControl/control</controlURL>"
                "<eventSubURL>/RenderingControl/event</eventSubURL>"
              "</service>"
              "<service>"
                "<serviceType>%s</serviceType>"
                "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
                "<SCPDURL>/ConnectionManager.xml</SCPDURL>"
                "<controlURL>/ConnectionManager/control</controlURL>"
                "<eventSubURL>/ConnectionManager/event</eventSubURL>"
              "</service>"
            "</serviceList>"
          "</device>"
        "</root>",
        DEVICE_TYPE, s_dlna.device_name, DEVICE_UUID,
        AV_TRANSPORT_TYPE, RENDER_CTRL_TYPE, CONN_MGR_TYPE);

    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

//--------------------------------------------------------------------+
// SOAP response helpers
//--------------------------------------------------------------------+

static void soap_ok(httpd_req_t *req, const char *service_ns,
                    const char *action, const char *body_extra)
{
    char resp[1024];
    snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
          "<s:Body>"
            "<u:%sResponse xmlns:u=\"%s\">"
              "%s"
            "</u:%sResponse>"
          "</s:Body>"
        "</s:Envelope>",
        action, service_ns,
        body_extra ? body_extra : "",
        action);

    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, resp, strlen(resp));
}

static void soap_error(httpd_req_t *req, int code, const char *desc)
{
    char resp[512];
    snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
          "<s:Body>"
            "<s:Fault>"
              "<faultcode>s:Client</faultcode>"
              "<faultstring>UPnPError</faultstring>"
              "<detail>"
                "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                  "<errorCode>%d</errorCode>"
                  "<errorDescription>%s</errorDescription>"
                "</UPnPError>"
              "</detail>"
            "</s:Fault>"
          "</s:Body>"
        "</s:Envelope>", code, desc);

    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, resp, strlen(resp));
}

//--------------------------------------------------------------------+
// Helper: extract XML element value (simple, no full parser)
//--------------------------------------------------------------------+

// Finds first occurrence of <tag>value</tag> and copies value to out.
// Returns true if found.
static bool xml_get(const char *xml, const char *tag, char *out, size_t out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag,  sizeof(open_tag),  "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (!end) return false;

    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

// Extract SOAP action name from SOAPAction header (format: "ns#ActionName")
static bool soap_get_action(httpd_req_t *req, char *action, size_t action_size)
{
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "SOAPAction") + 1;
    if (hdr_len < 2) return false;
    char *hdr = malloc(hdr_len);
    if (!hdr) return false;
    httpd_req_get_hdr_value_str(req, "SOAPAction", hdr, hdr_len);

    // Format: "urn:...:service#ActionName" or "urn:...:service#ActionName" with quotes
    char *hash = strrchr(hdr, '#');
    if (hash) {
        hash++;
        // Strip trailing quote if present
        char *end = strpbrk(hash, "\"'");
        if (end) *end = '\0';
        strncpy(action, hash, action_size - 1);
        action[action_size - 1] = '\0';
        free(hdr);
        return true;
    }
    free(hdr);
    return false;
}

//--------------------------------------------------------------------+
// AVTransport SOAP handler
//--------------------------------------------------------------------+

static esp_err_t handle_avtransport_control(httpd_req_t *req)
{
    char action[64] = {0};
    soap_get_action(req, action, sizeof(action));

    // Read body
    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_req_recv(req, body, req->content_len);
    body[req->content_len] = '\0';

    ESP_LOGD(TAG, "AVTransport: %s", action);

    if (strcmp(action, "SetAVTransportURI") == 0) {
        char uri[512] = {0};
        char metadata[512] = {0};
        xml_get(body, "CurrentURI", uri, sizeof(uri));
        xml_get(body, "CurrentURIMetaData", metadata, sizeof(metadata));

        strncpy(s_dlna.current_uri, uri, sizeof(s_dlna.current_uri) - 1);
        strncpy(s_dlna.current_metadata, metadata, sizeof(s_dlna.current_metadata) - 1);
        s_dlna.transport_state = DLNA_TRANSPORT_STOPPED;
        s_dlna.controller_connected = true;
        ESP_LOGI(TAG, "SetAVTransportURI: %s", uri);

        soap_ok(req, AV_TRANSPORT_TYPE, action, NULL);

    } else if (strcmp(action, "Play") == 0) {
        ESP_LOGI(TAG, "Play: %s", s_dlna.current_uri);
        s_dlna.transport_state = DLNA_TRANSPORT_TRANSITIONING;

        if (s_dlna.cbs.on_play && s_dlna.current_uri[0]) {
            s_dlna.cbs.on_play(s_dlna.current_uri, s_dlna.current_metadata);
        }
        s_dlna.transport_state = DLNA_TRANSPORT_PLAYING;
        soap_ok(req, AV_TRANSPORT_TYPE, action, NULL);

    } else if (strcmp(action, "Pause") == 0) {
        ESP_LOGI(TAG, "Pause");
        if (s_dlna.cbs.on_pause) s_dlna.cbs.on_pause();
        s_dlna.transport_state = DLNA_TRANSPORT_PAUSED;
        soap_ok(req, AV_TRANSPORT_TYPE, action, NULL);

    } else if (strcmp(action, "Stop") == 0) {
        ESP_LOGI(TAG, "Stop");
        if (s_dlna.cbs.on_stop) s_dlna.cbs.on_stop();
        s_dlna.transport_state = DLNA_TRANSPORT_STOPPED;
        soap_ok(req, AV_TRANSPORT_TYPE, action, NULL);

    } else if (strcmp(action, "Seek") == 0) {
        char target[32] = {0};
        xml_get(body, "Target", target, sizeof(target));
        // Target format: "0:01:30" (H:MM:SS)
        unsigned h = 0, m = 0, s_sec = 0;
        sscanf(target, "%u:%u:%u", &h, &m, &s_sec);
        uint32_t target_ms = ((h * 3600) + (m * 60) + s_sec) * 1000;
        if (s_dlna.cbs.on_seek) s_dlna.cbs.on_seek(target_ms);
        soap_ok(req, AV_TRANSPORT_TYPE, action, NULL);

    } else if (strcmp(action, "GetTransportInfo") == 0) {
        char extra[256];
        snprintf(extra, sizeof(extra),
            "<CurrentTransportState>%s</CurrentTransportState>"
            "<CurrentTransportStatus>OK</CurrentTransportStatus>"
            "<CurrentSpeed>1</CurrentSpeed>",
            transport_state_str(s_dlna.transport_state));
        soap_ok(req, AV_TRANSPORT_TYPE, action, extra);

    } else if (strcmp(action, "GetPositionInfo") == 0) {
        uint32_t cur_s = s_dlna.current_ms / 1000;
        uint32_t tot_s = s_dlna.total_ms / 1000;
        char extra[512];
        snprintf(extra, sizeof(extra),
            "<Track>1</Track>"
            "<TrackDuration>%u:%02u:%02u</TrackDuration>"
            "<TrackMetaData>%s</TrackMetaData>"
            "<TrackURI>%s</TrackURI>"
            "<RelTime>%u:%02u:%02u</RelTime>"
            "<AbsTime>%u:%02u:%02u</AbsTime>"
            "<RelCount>2147483647</RelCount>"
            "<AbsCount>2147483647</AbsCount>",
            tot_s / 3600, (tot_s % 3600) / 60, tot_s % 60,
            s_dlna.current_metadata,
            s_dlna.current_uri,
            cur_s / 3600, (cur_s % 3600) / 60, cur_s % 60,
            cur_s / 3600, (cur_s % 3600) / 60, cur_s % 60);
        soap_ok(req, AV_TRANSPORT_TYPE, action, extra);

    } else if (strcmp(action, "GetMediaInfo") == 0) {
        uint32_t tot_s = s_dlna.total_ms / 1000;
        char extra[512];
        snprintf(extra, sizeof(extra),
            "<NrTracks>1</NrTracks>"
            "<MediaDuration>%u:%02u:%02u</MediaDuration>"
            "<CurrentURI>%s</CurrentURI>"
            "<CurrentURIMetaData>%s</CurrentURIMetaData>"
            "<NextURI>NOT_IMPLEMENTED</NextURI>"
            "<NextURIMetaData>NOT_IMPLEMENTED</NextURIMetaData>"
            "<PlayMedium>NETWORK</PlayMedium>"
            "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
            "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
            tot_s / 3600, (tot_s % 3600) / 60, tot_s % 60,
            s_dlna.current_uri,
            s_dlna.current_metadata);
        soap_ok(req, AV_TRANSPORT_TYPE, action, extra);

    } else {
        // Unimplemented — return 401 (Invalid Action)
        soap_error(req, 401, "Invalid Action");
    }

    free(body);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// RenderingControl SOAP handler
//--------------------------------------------------------------------+

static esp_err_t handle_rendering_control(httpd_req_t *req)
{
    char action[64] = {0};
    soap_get_action(req, action, sizeof(action));

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_req_recv(req, body, req->content_len);
    body[req->content_len] = '\0';

    if (strcmp(action, "SetVolume") == 0) {
        char vol_str[8] = {0};
        xml_get(body, "DesiredVolume", vol_str, sizeof(vol_str));
        uint8_t vol = (uint8_t)atoi(vol_str);
        if (vol > 100) vol = 100;
        s_dlna.volume = vol;
        if (s_dlna.cbs.on_volume) s_dlna.cbs.on_volume(vol);
        soap_ok(req, RENDER_CTRL_TYPE, action, NULL);

    } else if (strcmp(action, "GetVolume") == 0) {
        char extra[64];
        snprintf(extra, sizeof(extra), "<CurrentVolume>%u</CurrentVolume>", s_dlna.volume);
        soap_ok(req, RENDER_CTRL_TYPE, action, extra);

    } else if (strcmp(action, "SetMute") == 0) {
        char mute_str[4] = {0};
        xml_get(body, "DesiredMute", mute_str, sizeof(mute_str));
        bool mute = (atoi(mute_str) != 0);
        s_dlna.mute = mute;
        if (s_dlna.cbs.on_mute) s_dlna.cbs.on_mute(mute);
        soap_ok(req, RENDER_CTRL_TYPE, action, NULL);

    } else if (strcmp(action, "GetMute") == 0) {
        char extra[32];
        snprintf(extra, sizeof(extra), "<CurrentMute>%d</CurrentMute>", (int)s_dlna.mute);
        soap_ok(req, RENDER_CTRL_TYPE, action, extra);

    } else {
        soap_error(req, 401, "Invalid Action");
    }

    free(body);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// ConnectionManager SOAP handler
//--------------------------------------------------------------------+

static esp_err_t handle_connection_manager(httpd_req_t *req)
{
    char action[64] = {0};
    soap_get_action(req, action, sizeof(action));

    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_req_recv(req, body, req->content_len);
    body[req->content_len] = '\0';

    if (strcmp(action, "GetProtocolInfo") == 0) {
        // Announce all supported audio formats
        const char *proto_info =
            "<Source></Source>"
            "<Sink>"
              "http-get:*:audio/flac:*,"
              "http-get:*:audio/x-flac:*,"
              "http-get:*:audio/mpeg:*,"
              "http-get:*:audio/mp3:*,"
              "http-get:*:audio/x-wav:*,"
              "http-get:*:audio/wav:*,"
              "http-get:*:audio/aac:*,"
              "http-get:*:audio/ogg:*"
            "</Sink>";
        soap_ok(req, CONN_MGR_TYPE, action, proto_info);

    } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        soap_ok(req, CONN_MGR_TYPE, action, "<ConnectionIDs>0</ConnectionIDs>");

    } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        const char *info =
            "<RcsID>0</RcsID>"
            "<AVTransportID>0</AVTransportID>"
            "<ProtocolInfo></ProtocolInfo>"
            "<PeerConnectionManager></PeerConnectionManager>"
            "<PeerConnectionID>-1</PeerConnectionID>"
            "<Direction>Input</Direction>"
            "<Status>OK</Status>";
        soap_ok(req, CONN_MGR_TYPE, action, info);

    } else {
        soap_error(req, 401, "Invalid Action");
    }

    free(body);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Event subscription (minimal — return 200 with SID)
//--------------------------------------------------------------------+

static esp_err_t handle_event_subscribe(httpd_req_t *req)
{
    // Return a dummy SID — full eventing not implemented yet
    httpd_resp_set_hdr(req, "SID", "uuid:lyra-event-sub-001");
    httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// Service description XML (minimal SCPD)
//--------------------------------------------------------------------+

static const char *MINIMAL_SCPD =
    "<?xml version=\"1.0\"?>"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
      "<specVersion><major>1</major><minor>0</minor></specVersion>"
      "<actionList/>"
      "<serviceStateTable/>"
    "</scpd>";

static esp_err_t handle_scpd(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_send(req, MINIMAL_SCPD, strlen(MINIMAL_SCPD));
    return ESP_OK;
}

//--------------------------------------------------------------------+
// HTTP server setup
//--------------------------------------------------------------------+

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = DLNA_HTTP_PORT;
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;

    if (httpd_start(&s_dlna.http_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    // Device description
    httpd_uri_t desc_uri = {
        .uri = "/description.xml", .method = HTTP_GET,
        .handler = handle_device_description
    };
    httpd_register_uri_handler(s_dlna.http_server, &desc_uri);

    // AVTransport control + events
    httpd_uri_t avt_ctrl = {
        .uri = "/AVTransport/control", .method = HTTP_POST,
        .handler = handle_avtransport_control
    };
    httpd_uri_t avt_event = {
        .uri = "/AVTransport/event", .method = HTTP_SUBSCRIBE,
        .handler = handle_event_subscribe
    };
    httpd_register_uri_handler(s_dlna.http_server, &avt_ctrl);
    httpd_register_uri_handler(s_dlna.http_server, &avt_event);

    // RenderingControl
    httpd_uri_t rc_ctrl = {
        .uri = "/RenderingControl/control", .method = HTTP_POST,
        .handler = handle_rendering_control
    };
    httpd_uri_t rc_event = {
        .uri = "/RenderingControl/event", .method = HTTP_SUBSCRIBE,
        .handler = handle_event_subscribe
    };
    httpd_register_uri_handler(s_dlna.http_server, &rc_ctrl);
    httpd_register_uri_handler(s_dlna.http_server, &rc_event);

    // ConnectionManager
    httpd_uri_t cm_ctrl = {
        .uri = "/ConnectionManager/control", .method = HTTP_POST,
        .handler = handle_connection_manager
    };
    httpd_uri_t cm_event = {
        .uri = "/ConnectionManager/event", .method = HTTP_SUBSCRIBE,
        .handler = handle_event_subscribe
    };
    httpd_register_uri_handler(s_dlna.http_server, &cm_ctrl);
    httpd_register_uri_handler(s_dlna.http_server, &cm_event);

    // Service description XMLs
    httpd_uri_t avt_scpd = {
        .uri = "/AVTransport.xml", .method = HTTP_GET,
        .handler = handle_scpd
    };
    httpd_uri_t rc_scpd = {
        .uri = "/RenderingControl.xml", .method = HTTP_GET,
        .handler = handle_scpd
    };
    httpd_uri_t cm_scpd = {
        .uri = "/ConnectionManager.xml", .method = HTTP_GET,
        .handler = handle_scpd
    };
    httpd_register_uri_handler(s_dlna.http_server, &avt_scpd);
    httpd_register_uri_handler(s_dlna.http_server, &rc_scpd);
    httpd_register_uri_handler(s_dlna.http_server, &cm_scpd);

    ESP_LOGI(TAG, "HTTP server started on port %d", DLNA_HTTP_PORT);
    return ESP_OK;
}

//--------------------------------------------------------------------+
// SSDP task (discovery + periodic alive)
//--------------------------------------------------------------------+

// Send a SSDP NOTIFY alive or byebye message
static void ssdp_send_notify(const char *nt, const char *usn, bool alive)
{
    char msg[1024];
    if (alive) {
        snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "LOCATION: http://%s:%d/description.xml\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:alive\r\n"
            "SERVER: ESP32-P4/1.0 UPnP/1.0 Lyra/1.0\r\n"
            "USN: %s\r\n\r\n",
            SSDP_MCAST_ADDR, SSDP_PORT,
            s_dlna.local_ip, DLNA_HTTP_PORT,
            nt, usn);
    } else {
        snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:byebye\r\n"
            "USN: %s\r\n\r\n",
            SSDP_MCAST_ADDR, SSDP_PORT,
            nt, usn);
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(SSDP_PORT),
    };
    inet_pton(AF_INET, SSDP_MCAST_ADDR, &dest.sin_addr);

    lwip_sendto(s_dlna.ssdp_sock, msg, strlen(msg), 0,
                (struct sockaddr *)&dest, sizeof(dest));
}

// Announce all UPnP device/service types
static void ssdp_announce(bool alive)
{
    char usn_dev[128], usn_avt[128], usn_rc[128], usn_cm[128], usn_root[128];
    snprintf(usn_root, sizeof(usn_root), "%s", DEVICE_UUID);
    snprintf(usn_dev, sizeof(usn_dev), "%s::%s", DEVICE_UUID, DEVICE_TYPE);
    snprintf(usn_avt, sizeof(usn_avt), "%s::%s", DEVICE_UUID, AV_TRANSPORT_TYPE);
    snprintf(usn_rc,  sizeof(usn_rc),  "%s::%s", DEVICE_UUID, RENDER_CTRL_TYPE);
    snprintf(usn_cm,  sizeof(usn_cm),  "%s::%s", DEVICE_UUID, CONN_MGR_TYPE);

    ssdp_send_notify("upnp:rootdevice",  usn_root, alive);
    ssdp_send_notify(DEVICE_UUID,        usn_root, alive);
    ssdp_send_notify(DEVICE_TYPE,        usn_dev,  alive);
    ssdp_send_notify(AV_TRANSPORT_TYPE,  usn_avt,  alive);
    ssdp_send_notify(RENDER_CTRL_TYPE,   usn_rc,   alive);
    ssdp_send_notify(CONN_MGR_TYPE,      usn_cm,   alive);
}

// Send M-SEARCH response to a specific controller
static void ssdp_send_response(const char *st, const char *usn,
                                struct sockaddr_in *dest)
{
    char msg[1024];
    snprintf(msg, sizeof(msg),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "DATE: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: ESP32-P4/1.0 UPnP/1.0 Lyra/1.0\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n\r\n",
        s_dlna.local_ip, DLNA_HTTP_PORT,
        st, usn);

    lwip_sendto(s_dlna.ssdp_sock, msg, strlen(msg), 0,
                (struct sockaddr *)dest, sizeof(*dest));
}

static void ssdp_task(void *arg)
{
    uint8_t recv_buf[1500];
    TickType_t last_alive = xTaskGetTickCount();

    // Send initial alive
    ssdp_announce(true);

    while (s_dlna.running) {
        // Check for M-SEARCH (non-blocking with 2s timeout)
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        int n = lwip_recvfrom(s_dlna.ssdp_sock, recv_buf, sizeof(recv_buf) - 1,
                              0, (struct sockaddr *)&src_addr, &src_len);

        if (n > 0) {
            recv_buf[n] = '\0';
            if (strncmp((char *)recv_buf, "M-SEARCH", 8) == 0) {
                // Extract ST header
                char st[128] = {0};
                const char *st_hdr = strstr((char *)recv_buf, "ST:");
                if (!st_hdr) st_hdr = strstr((char *)recv_buf, "st:");
                if (st_hdr) {
                    st_hdr += 3;
                    while (*st_hdr == ' ') st_hdr++;
                    size_t i;
                    for (i = 0; i < sizeof(st) - 1 && st_hdr[i] && st_hdr[i] != '\r'; i++) {
                        st[i] = st_hdr[i];
                    }
                    st[i] = '\0';
                }

                // Respond if we match the search target
                char usn[128];
                bool should_respond = false;

                if (strcmp(st, "ssdp:all") == 0 ||
                    strcmp(st, "upnp:rootdevice") == 0) {
                    snprintf(usn, sizeof(usn), "%s::upnp:rootdevice", DEVICE_UUID);
                    should_respond = true;
                } else if (strcmp(st, DEVICE_UUID) == 0) {
                    strncpy(usn, DEVICE_UUID, sizeof(usn) - 1);
                    should_respond = true;
                } else if (strcmp(st, DEVICE_TYPE) == 0) {
                    snprintf(usn, sizeof(usn), "%s::%s", DEVICE_UUID, DEVICE_TYPE);
                    should_respond = true;
                } else if (strcmp(st, AV_TRANSPORT_TYPE) == 0) {
                    snprintf(usn, sizeof(usn), "%s::%s", DEVICE_UUID, AV_TRANSPORT_TYPE);
                    should_respond = true;
                } else if (strcmp(st, RENDER_CTRL_TYPE) == 0) {
                    snprintf(usn, sizeof(usn), "%s::%s", DEVICE_UUID, RENDER_CTRL_TYPE);
                    should_respond = true;
                }

                if (should_respond) {
                    // Small random delay per UPnP spec (0-MX seconds)
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ssdp_send_response(st, usn, &src_addr);
                }
            }
        }

        // Periodic alive announcement
        TickType_t now = xTaskGetTickCount();
        if ((now - last_alive) >= pdMS_TO_TICKS(SSDP_ALIVE_INTERVAL_S * 1000)) {
            ssdp_announce(true);
            last_alive = now;
        }
    }

    ssdp_announce(false);  // Byebye on shutdown
    lwip_close(s_dlna.ssdp_sock);
    s_dlna.ssdp_sock = -1;
    vTaskDelete(NULL);
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

esp_err_t dlna_renderer_start(const char *device_name)
{
    if (s_dlna.running) return ESP_OK;

    memset(&s_dlna, 0, sizeof(s_dlna));
    strncpy(s_dlna.device_name, device_name, sizeof(s_dlna.device_name) - 1);
    s_dlna.volume = 80;
    s_dlna.transport_state = DLNA_TRANSPORT_NO_MEDIA;

    // Get local IP
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) {
        ESP_LOGE(TAG, "No default netif — is WiFi connected?");
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    snprintf(s_dlna.local_ip, sizeof(s_dlna.local_ip), IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Local IP: %s", s_dlna.local_ip);

    // Create SSDP UDP socket
    s_dlna.ssdp_sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dlna.ssdp_sock < 0) {
        ESP_LOGE(TAG, "SSDP socket failed");
        return ESP_FAIL;
    }

    // Set socket timeout (2s) so SSDP task can do periodic alive
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    lwip_setsockopt(s_dlna.ssdp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind to SSDP port
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (lwip_bind(s_dlna.ssdp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "SSDP bind failed (port %d in use?) — continuing", SSDP_PORT);
    }

    // Join SSDP multicast group
    struct ip_mreq mreq = {0};
    inet_pton(AF_INET, SSDP_MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    lwip_setsockopt(s_dlna.ssdp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Set multicast TTL
    int ttl = 4;
    lwip_setsockopt(s_dlna.ssdp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Start HTTP server
    esp_err_t err = start_http_server();
    if (err != ESP_OK) {
        lwip_close(s_dlna.ssdp_sock);
        return err;
    }

    s_dlna.running = true;

    // Start SSDP task on CPU0 (network side)
    xTaskCreatePinnedToCore(ssdp_task, "dlna_ssdp", 4096, NULL, 3,
                            &s_dlna.ssdp_task_handle, 0);

    ESP_LOGI(TAG, "DLNA renderer '%s' started (http://%s:%d)",
             device_name, s_dlna.local_ip, DLNA_HTTP_PORT);
    return ESP_OK;
}

void dlna_renderer_stop(void)
{
    if (!s_dlna.running) return;
    s_dlna.running = false;

    if (s_dlna.http_server) {
        httpd_stop(s_dlna.http_server);
        s_dlna.http_server = NULL;
    }
    // SSDP task will exit on its own (checks s_dlna.running)
}

void dlna_register_callbacks(const dlna_control_cbs_t *cbs)
{
    if (cbs) memcpy(&s_dlna.cbs, cbs, sizeof(s_dlna.cbs));
}

void dlna_set_position(uint32_t current_ms, uint32_t total_ms)
{
    s_dlna.current_ms = current_ms;
    s_dlna.total_ms   = total_ms;
}

void dlna_set_transport_state(dlna_transport_state_t state)
{
    s_dlna.transport_state = state;
}

bool dlna_is_active(void)
{
    return s_dlna.running && s_dlna.controller_connected;
}

void dlna_notify_pause(void)
{
    s_dlna.transport_state = DLNA_TRANSPORT_PAUSED;
    // Full GENA eventing (sending NOTIFY to subscriber) is a TODO
    // For now: state is updated — controller will see it on next GetTransportInfo poll
}

void dlna_notify_play(void)
{
    s_dlna.transport_state = DLNA_TRANSPORT_PLAYING;
}
