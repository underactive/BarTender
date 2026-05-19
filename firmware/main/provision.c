// firmware/main/provision.c
#include "provision.h"
#include "config_store.h"
#include "ui.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "prov";
static httpd_handle_t s_http;
// true => Upstash already in NVS: serve the WiFi-only form and keep Upstash.
// The token is never written into any HTML response in this mode.
static bool s_wifi_only;

// ---- tiny x-www-form-urlencoded helpers ------------------------------------

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void urldecode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            int h = hexv(p[1]), l = hexv(p[2]);
            if (h >= 0 && l >= 0) { *o++ = (char)((h << 4) | l); p += 2; }
            else *o++ = *p;
        } else *o++ = *p;
    }
    *o = '\0';
}

// Extract field `name` from a urlencoded body into out (NUL-terminated).
static void field(const char *body, const char *name, char *out, size_t n)
{
    out[0] = '\0';
    char pat[24];
    snprintf(pat, sizeof pat, "%s=", name);
    const char *k = strstr(body, pat);
    if (!k) return;
    if (k != body && k[-1] != '&') {           // avoid matching a substring
        while ((k = strstr(k + 1, pat)))
            if (k[-1] == '&') break;
        if (!k) return;
    }
    k += strlen(pat);
    const char *e = strchr(k, '&');
    size_t len = e ? (size_t)(e - k) : strlen(k);
    if (len >= n) len = n - 1;
    memcpy(out, k, len);
    out[len] = '\0';
    urldecode(out);
}

// ---- HTTP handlers ---------------------------------------------------------

static const char FORM[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CodexBar Toy setup</title>"
    "<style>body{font-family:system-ui;margin:24px;max-width:480px}"
    "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}"
    "button{padding:10px 16px;font-size:16px}</style>"
    "<h2>CodexBar Toy setup</h2>"
    "<form method=POST action=/save>"
    "WiFi network<input name=ssid required>"
    "WiFi password<input name=pass type=password>"
    "Upstash REST URL<input name=url placeholder='https://xxx.upstash.io' required>"
    "Redis key<input name=key value='codexbar'>"
    "Upstash READ-ONLY token<input name=token required>"
    "<button type=submit>Save &amp; reboot</button></form>";

// WiFi-only variant: Upstash is already provisioned, so its fields are OMITTED
// entirely. No stored secret (URL/key/token) is ever interpolated here.
static const char FORM_WIFI[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CodexBar Toy — add WiFi</title>"
    "<style>body{font-family:system-ui;margin:24px;max-width:480px}"
    "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}"
    "button{padding:10px 16px;font-size:16px}</style>"
    "<h2>Add a WiFi network</h2>"
    "<p>Upstash is already set up — just add this location's WiFi. "
    "Up to 5 networks are remembered; the device autoconnects to whichever "
    "is in range.</p>"
    "<form method=POST action=/save>"
    "WiFi network<input name=ssid required>"
    "WiFi password<input name=pass type=password>"
    "<button type=submit>Save &amp; reboot</button></form>";

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, s_wifi_only ? FORM_WIFI : FORM,
                           HTTPD_RESP_USE_STRLEN);
}

// Captive-portal probes (Apple/Android/Windows) -> redirect to the form so the
// OS pops the "sign in to network" sheet.
static esp_err_t h_redirect(httpd_req_t *r)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static void reboot_task(void *a) { (void)a; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

// Audit Security§HIGH: read EXACTLY Content-Length so an over-long body is
// rejected with 413 instead of silently truncated (which previously persisted
// a corrupted token/url to NVS while replying "Saved"). 1536 B covers all
// five fields percent-encoded; CFG_*_MAX sum + names + %XX overhead < 1536.
static esp_err_t h_save(httpd_req_t *r)
{
    char buf[1536];
    size_t clen = r->content_len;
    if (clen == 0 || clen >= sizeof(buf)) {
        httpd_resp_set_status(r, "413 Payload Too Large");
        httpd_resp_set_type(r, "text/html");
        httpd_resp_send(r, "<h3>Form too large or empty — go back.</h3>",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    int total = 0;
    while ((size_t)total < clen) {
        int n = httpd_req_recv(r, buf + total, clen - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;     // transient — retry
        if (n <= 0) { httpd_resp_send_500(r); return ESP_FAIL; }  // hard error
        total += n;
    }
    buf[total] = '\0';

    char ssid[CFG_SSID_MAX], pass[CFG_PASS_MAX], url[CFG_URL_MAX],
         key[CFG_KEY_MAX], tok[CFG_TOKEN_MAX];
    field(buf, "ssid", ssid, sizeof ssid);
    field(buf, "pass", pass, sizeof pass);

    bool ok;
    if (s_wifi_only) {
        // Add/replace just this network (LRU rotate); Upstash is untouched.
        if (!ssid[0]) {
            httpd_resp_set_type(r, "text/html");
            httpd_resp_send(r, "<h3>Missing WiFi network — go back.</h3>",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ok = config_store_wifi_add_or_update(ssid, pass);
    } else {
        field(buf, "url",  url,  sizeof url);
        field(buf, "key",  key,  sizeof key);
        field(buf, "token", tok, sizeof tok);
        if (!ssid[0] || !url[0] || !tok[0]) {
            httpd_resp_set_type(r, "text/html");
            httpd_resp_send(r, "<h3>Missing required field — go back.</h3>",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ok  = config_store_set_upstash(url, key, tok);
        ok &= config_store_wifi_add_or_update(ssid, pass);
    }
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, ok ? "<h3>Saved. Rebooting…</h3>"
                          : "<h3>Save failed. Power-cycle and retry.</h3>",
                    HTTPD_RESP_USE_STRLEN);
    if (ok) xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---- captive DNS: answer every A query with 192.168.4.1 --------------------

static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { ESP_LOGE(TAG, "dns socket"); vTaskDelete(NULL); return; }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "dns bind"); close(s); vTaskDelete(NULL); return;
    }
    uint8_t pkt[512];
    while (1) {
        struct sockaddr_in src; socklen_t sl = sizeof src;
        int len = recvfrom(s, pkt, sizeof pkt, 0, (struct sockaddr *)&src, &sl);
        // Audit Security§CRITICAL: validate the query before forging a reply.
        if (len < 12) continue;
        if (pkt[2] & 0x80) continue;                 // drop responses (no reflection loop)
        if (pkt[4] != 0 || pkt[5] != 1) continue;    // require exactly one question
        // Walk QNAME labels to find the true question end (no assumed offset).
        int q = 12;
        while (q < len && pkt[q] != 0) {
            if (pkt[q] & 0xC0) { q = -1; break; }    // compression illegal in a query
            q += pkt[q] + 1;
        }
        if (q < 0 || q + 5 > len) continue;          // malformed / truncated
        q += 1;                                       // skip the root (zero) label
        int qtype = (pkt[q] << 8) | pkt[q + 1];
        int qend  = q + 4;                            // + QTYPE(2) + QCLASS(2)
        pkt[2] |= 0x80; pkt[3] = 0x80;               // QR=1, RA=1, RCODE=0
        pkt[6] = 0; pkt[7] = (qtype == 1) ? 1 : 0;   // ANCOUNT: A→1, else 0
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;      // NS=AR=0 (drop EDNS/extra)
        int out = qend;                               // truncate trailing records
        if (qtype == 1) {                             // answer A with 192.168.4.1
            if (qend + 16 > (int)sizeof pkt) continue;
            uint8_t *p = pkt + qend;
            *p++ = 0xC0; *p++ = 0x0C;                // ptr → QNAME at offset 12
            *p++ = 0x00; *p++ = 0x01;                // TYPE A
            *p++ = 0x00; *p++ = 0x01;                // CLASS IN
            *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 60; // TTL 60
            *p++ = 0x00; *p++ = 0x04;                // RDLENGTH 4
            *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1;
            out = qend + 16;
        }
        sendto(s, pkt, out, 0, (struct sockaddr *)&src, sl);
    }
}

// ---- bring-up --------------------------------------------------------------

void provision_start(bool upstash_already_set)
{
    s_wifi_only = upstash_already_set;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33], pass[16];
    snprintf(ssid, sizeof ssid, "CodexBar-Toy-%02X%02X", mac[4], mac[5]);
    snprintf(pass, sizeof pass, "cbtoy-%02X%02X%02X", mac[3], mac[4], mac[5]); // >=8, WPA2

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, ssid, sizeof wc.ap.ssid);
    wc.ap.ssid_len = strlen(ssid);
    strlcpy((char *)wc.ap.password, pass, sizeof wc.ap.password);
    wc.ap.channel = 1;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    // h_save uses char buf[1536] + ~530 B of field buffers AND calls into
    // config_store_wifi_add_or_update, which puts a ~490 B wifi_creds_t on
    // the stack, then deep NVS→partition→flash→cache-disable. The default
    // 4 KB httpd task stack overflows there (corrupts the TCB → a bogus
    // xTaskPriorityDisinherit assert in spi_flash_op_unlock). Give it room.
    hc.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_http, &hc));
    httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u_any  = { .uri = "/*",    .method = HTTP_GET,  .handler = h_redirect };
    httpd_register_uri_handler(s_http, &u_root);
    httpd_register_uri_handler(s_http, &u_save);
    httpd_register_uri_handler(s_http, &u_any);

    xTaskCreate(dns_task, "captdns", 3072, NULL, 4, NULL);

    // Audit Security§LOW: don't print the AP PSK to the UART log — it's shown
    // on the device screen for the legitimate user via ui_set_provisioning().
    ESP_LOGW(TAG, "PROVISIONING AP up (%s): SSID=%s (password on screen) -> http://192.168.4.1/",
             s_wifi_only ? "add-wifi" : "first-boot", ssid);
    ui_set_provisioning(ssid, pass, s_wifi_only);
}
