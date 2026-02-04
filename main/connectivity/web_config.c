#include "web_config.h"

#include "config_store.h"
#include "config_owner.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_ntp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_config";
static httpd_handle_t s_server = NULL;

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static void url_decode_range(const char *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t di = 0;
    for (size_t i = 0; i < src_len && di + 1 < dst_len; ++i) {
        char ch = src[i];
        if (ch == '+') {
            dst[di++] = ' ';
        } else if (ch == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[di++] = ch;
            }
        } else {
            dst[di++] = ch;
        }
    }
    dst[di] = '\0';
}

static void html_escape(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;
    for (size_t i = 0; src && src[i] != '\0' && di + 1 < dst_len; ++i) {
        char ch = src[i];
        const char *rep = NULL;
        if (ch == '&') {
            rep = "&amp;";
        } else if (ch == '<') {
            rep = "&lt;";
        } else if (ch == '>') {
            rep = "&gt;";
        } else if (ch == '"') {
            rep = "&quot;";
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            if (di + rep_len >= dst_len) {
                break;
            }
            memcpy(&dst[di], rep, rep_len);
            di += rep_len;
        } else {
            dst[di++] = ch;
        }
    }
    dst[di] = '\0';
}

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool is_fetch_request(httpd_req_t *req)
{
    char hdr[64];
    if (httpd_req_get_hdr_value_str(req, "X-Requested-With", hdr, sizeof(hdr)) == ESP_OK) {
        if (str_ieq(hdr, "fetch") || str_ieq(hdr, "xhr")) {
            return true;
        }
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", hdr, sizeof(hdr)) == ESP_OK) {
        if (strstr(hdr, "application/json")) {
            return true;
        }
    }
    return false;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) {
        return false;
    }

    size_t key_len = strlen(key);
    const char *pos = body;
    while (pos && *pos) {
        const char *eq = strchr(pos, '=');
        if (!eq) {
            break;
        }
        size_t name_len = (size_t)(eq - pos);
        const char *amp = strchr(eq + 1, '&');
        if (name_len == key_len && strncmp(pos, key, key_len) == 0) {
            size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            url_decode_range(eq + 1, val_len, out, out_len);
            return true;
        }
        if (!amp) {
            break;
        }
        pos = amp + 1;
    }
    return false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/wifi");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    app_config_t cfg;
    config_store_get(&cfg);

    const char *status = wifi_is_connected() ? "connected" : "disconnected";
    const char *mode = wifi_is_ap_mode() ? "AP" : "STA";

    char ssid_esc[64];
    html_escape(cfg.wifi_ssid, ssid_esc, sizeof(ssid_esc));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta charset=\"utf-8\">"
                             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                             "<title>Clock WiFi</title>"
                             "<style>"
                             "body{font-family:Arial,sans-serif;margin:20px;color:#111;background:#f7f7f7;}"
                             ".card{background:#fff;border:1px solid #ddd;border-radius:8px;padding:14px;margin-bottom:12px;}"
                             "label{display:block;font-size:12px;color:#555;margin-bottom:6px;}"
                             "input{width:100%;padding:8px;border:1px solid #ccc;border-radius:6px;}"
                             "button{padding:8px 12px;border-radius:6px;border:1px solid #888;background:#eee;}"
                             ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}"
                             "@media(max-width:520px){.row{grid-template-columns:1fr;}}"
                             "</style></head><body>");

    char chunk[512];
    snprintf(chunk, sizeof(chunk),
             "<div class=\"card\"><strong>WiFi:</strong> %s (%s)</div>",
             status, mode);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "<form class=\"card\" method=\"post\" action=\"/wifi\">"
             "<div class=\"row\">"
             "<div><label>SSID</label><input name=\"ssid\" value=\"%s\" maxlength=\"31\"></div>"
             "<div><label>Password</label><input name=\"pass\" type=\"password\" value=\"\" maxlength=\"63\" placeholder=\"(unchanged)\"></div>"
             "</div>"
             "<p style=\"font-size:12px;color:#666;\">Clear SSID to enable AP mode.</p>"
             "<button type=\"submit\">Save WiFi</button>"
             "</form>",
             ssid_esc);
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
                             "<form class=\"card\" method=\"post\" action=\"/wifi_reset\">"
                             "<button type=\"submit\">Reset WiFi</button>"
                             "</form>"
                             "</body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    const size_t max_len = 512;
    size_t total = req->content_len;
    if (total == 0 || total > max_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_FAIL;
    }

    char *body = calloc(1, total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[total] = '\0';

    char ssid[32] = {0};
    char pass[64] = {0};
    bool has_ssid = form_get_value(body, "ssid", ssid, sizeof(ssid));
    bool has_pass = form_get_value(body, "pass", pass, sizeof(pass));
    free(body);

    app_config_t cfg;
    config_store_get(&cfg);
    char prev_ssid[32];
    char prev_pass[64];
    strncpy(prev_ssid, cfg.wifi_ssid, sizeof(prev_ssid) - 1);
    prev_ssid[sizeof(prev_ssid) - 1] = '\0';
    strncpy(prev_pass, cfg.wifi_pass, sizeof(prev_pass) - 1);
    prev_pass[sizeof(prev_pass) - 1] = '\0';

    char new_ssid[32];
    char new_pass[64];
    strncpy(new_ssid, cfg.wifi_ssid, sizeof(new_ssid) - 1);
    new_ssid[sizeof(new_ssid) - 1] = '\0';
    strncpy(new_pass, cfg.wifi_pass, sizeof(new_pass) - 1);
    new_pass[sizeof(new_pass) - 1] = '\0';

    if (has_ssid) {
        strncpy(new_ssid, ssid, sizeof(new_ssid) - 1);
        new_ssid[sizeof(new_ssid) - 1] = '\0';
    }
    if (has_pass) {
        if (pass[0] != '\0') {
            strncpy(new_pass, pass, sizeof(new_pass) - 1);
            new_pass[sizeof(new_pass) - 1] = '\0';
        } else if (strcmp(new_ssid, cfg.wifi_ssid) != 0) {
            new_pass[0] = '\0';
        }
    }

    strncpy(cfg.wifi_ssid, new_ssid, sizeof(cfg.wifi_ssid) - 1);
    cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    strncpy(cfg.wifi_pass, new_pass, sizeof(cfg.wifi_pass) - 1);
    cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';

    if (!config_owner_request_update(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    bool wifi_changed = (strcmp(prev_ssid, cfg.wifi_ssid) != 0) || (strcmp(prev_pass, cfg.wifi_pass) != 0);
    if (wifi_changed) {
        wifi_update_credentials(cfg.wifi_ssid, cfg.wifi_pass);
    }

    if (is_fetch_request(req)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *resp =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Clock WiFi</title></head><body>"
        "<p>Saved. Reconnecting...</p>"
        "<p><a href=\"/wifi\">Back</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    app_config_t cfg;
    config_store_get(&cfg);
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_pass[0] = '\0';

    if (!config_owner_request_update(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    wifi_update_credentials("", "");

    if (is_fetch_request(req)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *resp =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Clock WiFi</title></head><body>"
        "<p>WiFi reset. AP mode enabled.</p>"
        "<p><a href=\"/wifi\">Back</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t web_config_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 4096;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t wifi_get = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &wifi_get);

    httpd_uri_t wifi_post = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &wifi_post);

    httpd_uri_t wifi_reset = {
        .uri = "/wifi_reset",
        .method = HTTP_POST,
        .handler = wifi_reset_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &wifi_reset);

    ESP_LOGI(TAG, "web config server started");
    return ESP_OK;
}

esp_err_t web_config_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd stop failed: %s", esp_err_to_name(err));
        return err;
    }
    s_server = NULL;
    ESP_LOGI(TAG, "httpd stopped");
    return ESP_OK;
}
