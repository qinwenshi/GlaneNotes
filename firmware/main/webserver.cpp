// webserver.cpp — Glane Notes local dashboard served from the device.
//
// Routes:
//   GET  /            note list (audio + transcript status)
//   GET  /style.css   stylesheet
//   GET  /note?id=    view one note's transcript + audio player
//   GET  /dl?id=      download the WAV (chunked from SD)
//   POST /del?id=     delete a note (wav + txt)
//   GET  /settings    Wi-Fi + API key form
//   POST /settings    save settings
//   POST /sync        enqueue a sync job (returns immediately)

#include "webserver.h"
#include "glane_config.h"
#include "settings.h"
#include "sync.h"
#include "wifi_mgr.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "web";
static httpd_handle_t       s_server = nullptr;
static web_sync_request_cb_t s_on_sync = nullptr;

// ── Helpers ──────────────────────────────────────────────────────────────────
static void send_chunk(httpd_req_t *r, const char *s) { httpd_resp_sendstr_chunk(r, s); }

// URL-decode in place (handles %XX and '+').
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            *o++ = (char)((hex(p[1]) << 4) | hex(p[2]));
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// Extract a query/body arg. Returns true if found.
static bool get_param(httpd_req_t *r, const char *src, const char *key, char *out, size_t n)
{
    if (!src) return false;
    if (httpd_query_key_value(src, key, out, n) == ESP_OK) {
        url_decode(out);
        return true;
    }
    return false;
}

// Validate an id is a safe basename (no slashes / dot traversal).
static bool id_is_safe(const char *id)
{
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
        if (*p == '.' && p[1] == '.') return false;
    }
    return strlen(id) < 40;
}

// HTML-escape into a chunk write.
static void send_escaped(httpd_req_t *r, const char *s)
{
    char buf[256];
    size_t b = 0;
    for (const char *p = s; *p; p++) {
        const char *rep = nullptr;
        switch (*p) {
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '&': rep = "&amp;"; break;
            case '"': rep = "&quot;"; break;
        }
        if (rep) {
            if (b) { buf[b] = 0; send_chunk(r, buf); b = 0; }
            send_chunk(r, rep);
        } else {
            buf[b++] = *p;
            if (b >= sizeof(buf) - 1) { buf[b] = 0; send_chunk(r, buf); b = 0; }
        }
    }
    if (b) { buf[b] = 0; send_chunk(r, buf); }
}

// ── Shared chrome ────────────────────────────────────────────────────────────
static const char kStyle[] =
    "body{margin:0;background:#f3efe7;color:#1f2328;font:15px/1.5 system-ui,sans-serif}"
    ".wrap{max-width:760px;margin:0 auto;padding:18px}"
    "h1{font-size:22px;margin:0 0 2px}.muted{color:#667085;font-size:13px}"
    "a{color:#3c5a7a;text-decoration:none}a:hover{text-decoration:underline}"
    ".nav{display:flex;gap:14px;margin:10px 0 16px;font-size:14px}"
    ".card{background:#fff;border:1px solid #ddd4c7;border-radius:14px;padding:14px 16px;margin:0 0 14px}"
    ".note{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;"
    "padding:12px 0;border-top:1px solid #ece5d9}.note:first-child{border-top:0}"
    ".pill{display:inline-block;border-radius:999px;padding:2px 9px;font-size:12px;background:#f6f2ea;color:#6b6358}"
    ".pill.ok{background:#e7f6ec;color:#216e39}.pill.no{background:#fff4d6;color:#8a5a00}"
    "button,.btn{border:0;border-radius:10px;background:#1f2328;color:#fff;padding:9px 14px;"
    "font:600 14px system-ui;cursor:pointer;text-decoration:none;display:inline-block}"
    ".btn.sec{background:#eef2f6;color:#334e68;border:1px solid #d8e0e8}"
    ".btn.danger{background:#6e2a2a}"
    "input{width:100%;box-sizing:border-box;border:1px solid #c9c2b8;border-radius:10px;padding:10px;font:inherit;margin:4px 0 12px}"
    "label{font-weight:600;font-size:14px}"
    "pre{white-space:pre-wrap;background:#fcfaf7;border:1px solid #ddd4c7;border-radius:10px;padding:12px}"
    ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px}";

static void page_head(httpd_req_t *r, const char *title)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    send_chunk(r, "<!doctype html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<link rel='stylesheet' href='/style.css'><title>");
    send_chunk(r, title);
    send_chunk(r, "</title></head><body><div class='wrap'>");
    send_chunk(r, "<h1>Glane Notes</h1><div class='muted'>Voice notes, captured on the device.</div>");
    send_chunk(r, "<div class='nav'><a href='/'>Notes</a><a href='/settings'>Settings</a>"
                  "<form method='POST' action='/sync' style='margin:0'>"
                  "<button class='btn sec' style='padding:2px 10px;font-size:13px'>Sync now</button></form></div>");
}

static void page_end(httpd_req_t *r)
{
    send_chunk(r, "</div></body></html>");
    httpd_resp_sendstr_chunk(r, nullptr);
}

// ── GET /style.css ───────────────────────────────────────────────────────────
static esp_err_t h_style(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/css");
    httpd_resp_set_hdr(r, "Cache-Control", "public, max-age=3600");
    httpd_resp_sendstr(r, kStyle);
    return ESP_OK;
}

// ── GET / ────────────────────────────────────────────────────────────────────
static esp_err_t h_index(httpd_req_t *r)
{
    static note_info_t list[MAX_NOTES];
    int n = notes_scan(list, MAX_NOTES);

    page_head(r, "Glane Notes");

    char line[128];
    int pending = 0;
    for (int i = 0; i < n; i++) if (!list[i].has_text) pending++;
    snprintf(line, sizeof(line),
             "<div class='card'><b>%d</b> notes &nbsp;·&nbsp; <b>%d</b> awaiting transcription</div>",
             n, pending);
    send_chunk(r, line);

    if (n == 0) {
        send_chunk(r, "<div class='card muted'>No notes yet. Press the button on the device to record.</div>");
    } else {
        send_chunk(r, "<div class='card'>");
        for (int i = 0; i < n; i++) {
            send_chunk(r, "<div class='note'><div>");
            send_chunk(r, "<div><a href='/note?id=");
            send_chunk(r, list[i].id);
            send_chunk(r, "'>");
            send_escaped(r, list[i].id);
            send_chunk(r, "</a></div>");
            snprintf(line, sizeof(line), "<div class='muted'>%u KB</div></div><div>",
                     (unsigned)(list[i].wav_bytes / 1024));
            send_chunk(r, line);
            send_chunk(r, list[i].has_text ? "<span class='pill ok'>text</span>"
                                           : "<span class='pill no'>pending</span>");
            send_chunk(r, "</div></div>");
        }
        send_chunk(r, "</div>");
    }
    page_end(r);
    return ESP_OK;
}

// ── GET /note?id= ────────────────────────────────────────────────────────────
static esp_err_t h_note(httpd_req_t *r)
{
    char q[128] = {0}, id[48] = {0};
    httpd_req_get_url_query_str(r, q, sizeof(q));
    get_param(r, q, "id", id, sizeof(id));
    if (!id_is_safe(id)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad id"); return ESP_OK; }

    page_head(r, "Note");
    send_chunk(r, "<div class='card'><h1 style='font-size:18px'>");
    send_escaped(r, id);
    send_chunk(r, "</h1>");

    send_chunk(r, "<audio controls preload='none' style='width:100%;margin:10px 0' src='/dl?id=");
    send_chunk(r, id);
    send_chunk(r, "'></audio>");

    char *txt = notes_read_text(id);
    if (txt) {
        send_chunk(r, "<pre>");
        send_escaped(r, txt);
        send_chunk(r, "</pre>");
        free(txt);
    } else {
        send_chunk(r, "<div class='muted'>No transcript yet. Run a sync to transcribe.</div>");
    }

    send_chunk(r, "<div class='actions'><a class='btn sec' href='/dl?id=");
    send_chunk(r, id);
    send_chunk(r, "'>Download audio</a>"
                  "<form method='POST' action='/del?id=");
    send_chunk(r, id);
    send_chunk(r, "' onsubmit='return confirm(\"Delete this note?\")' style='margin:0'>"
                  "<button class='btn danger'>Delete</button></form></div>");
    send_chunk(r, "<div style='margin-top:12px'><a href='/'>&larr; All notes</a></div></div>");
    page_end(r);
    return ESP_OK;
}

// ── GET /dl?id= (chunked WAV) ────────────────────────────────────────────────
static esp_err_t h_download(httpd_req_t *r)
{
    char q[128] = {0}, id[48] = {0};
    httpd_req_get_url_query_str(r, q, sizeof(q));
    get_param(r, q, "id", id, sizeof(id));
    if (!id_is_safe(id)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad id"); return ESP_OK; }

    char path[160];
    snprintf(path, sizeof(path), "%s/%s.wav", NOTES_DIR, id);
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no audio"); return ESP_OK; }

    httpd_resp_set_type(r, "audio/wav");
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.wav\"", id);
    httpd_resp_set_hdr(r, "Content-Disposition", disp);

    char *buf = (char *)malloc(4096);
    if (!buf) { fclose(f); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    size_t rd;
    while ((rd = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, rd) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(r, nullptr, 0);
    return ESP_OK;
}

// ── POST /del?id= ────────────────────────────────────────────────────────────
static esp_err_t h_delete(httpd_req_t *r)
{
    char q[128] = {0}, id[48] = {0};
    httpd_req_get_url_query_str(r, q, sizeof(q));
    get_param(r, q, "id", id, sizeof(id));
    if (!id_is_safe(id)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad id"); return ESP_OK; }

    char path[160];
    snprintf(path, sizeof(path), "%s/%s.wav", NOTES_DIR, id); remove(path);
    snprintf(path, sizeof(path), "%s/%s.txt", NOTES_DIR, id); remove(path);

    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/");
    httpd_resp_send(r, nullptr, 0);
    return ESP_OK;
}

// ── GET/POST /settings ───────────────────────────────────────────────────────
static esp_err_t h_settings_get(httpd_req_t *r)
{
    const glane_settings_t *cfg = settings_get();
    page_head(r, "Settings");
    send_chunk(r, "<form method='POST' action='/settings'><div class='card'>");

    send_chunk(r, "<label>Wi-Fi network (SSID)</label><input name='ssid' value='");
    send_escaped(r, cfg->wifi_ssid);
    send_chunk(r, "'>");

    send_chunk(r, "<label>Wi-Fi password</label><input name='pass' type='password' placeholder='(unchanged)'>");

    send_chunk(r, "<label>DashScope API key</label><input name='apikey' type='password' placeholder='");
    send_chunk(r, settings_has_api_key() ? "(set — leave blank to keep)" : "sk-...");
    send_chunk(r, "'>");

    send_chunk(r, "<div class='muted'>Used to transcribe notes via Aliyun qwen3-asr-flash. "
                  "Audio stays on the SD card; only sync uploads it.</div>");

    send_chunk(r, "<label>Inbox webhook URL</label><input name='inboxurl' value='");
    send_escaped(r, cfg->inbox_url);
    send_chunk(r, "' placeholder='https://your-worker.workers.dev/inbox'>");

    send_chunk(r, "<label>Inbox token</label><input name='inboxtok' type='password' placeholder='");
    send_chunk(r, settings_has_inbox() && cfg->inbox_token[0] ? "(set — leave blank to keep)" : "bearer token");
    send_chunk(r, "'>");

    send_chunk(r, "<div class='muted'>Optional: after transcription, each note is pushed "
                  "to this webhook (a Cloudflare Worker → Notion inbox). Leave the URL blank to disable.</div>");
    send_chunk(r, "<div class='actions'><button>Save</button></div></div></form>");

    char ipline[96];
    snprintf(ipline, sizeof(ipline), "<div class='card muted'>Status: %s%s</div>",
             wifi_mgr_is_connected() ? "Wi-Fi connected " : "Wi-Fi offline",
             wifi_mgr_is_connected() ? wifi_mgr_ip() : "");
    send_chunk(r, ipline);
    page_end(r);
    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *r, char *buf, size_t n)
{
    int total = r->content_len;
    if (total <= 0 || (size_t)total >= n) return ESP_FAIL;
    int got = 0;
    while (got < total) {
        int k = httpd_req_recv(r, buf + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t h_settings_post(httpd_req_t *r)
{
    char body[1024];
    if (read_body(r, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    char ssid[96] = {0}, pass[96] = {0}, key[128] = {0};
    char inboxurl[160] = {0}, inboxtok[96] = {0};
    bool has_ssid = get_param(r, body, "ssid", ssid, sizeof(ssid));
    bool has_pass = get_param(r, body, "pass", pass, sizeof(pass));
    bool has_key  = get_param(r, body, "apikey", key, sizeof(key));
    bool has_url  = get_param(r, body, "inboxurl", inboxurl, sizeof(inboxurl));
    bool has_tok  = get_param(r, body, "inboxtok", inboxtok, sizeof(inboxtok));

    if (has_ssid && ssid[0]) {
        // If password left blank, keep the stored one.
        const char *p = (has_pass && pass[0]) ? pass : settings_get()->wifi_pass;
        settings_set_wifi(ssid, p);
    }
    if (has_key && key[0]) {
        settings_set_api_key(key);
    }
    if (has_url) {
        // Clearing the URL disables the inbox. Blank token keeps the stored one.
        const char *t = (has_tok && inboxtok[0]) ? inboxtok : settings_get()->inbox_token;
        settings_set_inbox(inboxurl, t);
    }

    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/settings");
    httpd_resp_send(r, nullptr, 0);
    return ESP_OK;
}

// ── POST /sync ───────────────────────────────────────────────────────────────
static esp_err_t h_sync(httpd_req_t *r)
{
    if (s_on_sync) s_on_sync();
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/");
    httpd_resp_send(r, nullptr, 0);
    return ESP_OK;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
void webserver_start(web_sync_request_cb_t on_sync_request)
{
    if (s_server) return;
    s_on_sync = on_sync_request;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = nullptr;
        return;
    }

    httpd_uri_t routes[] = {
        { "/style.css", HTTP_GET,  h_style,         nullptr },
        { "/",          HTTP_GET,  h_index,         nullptr },
        { "/note",      HTTP_GET,  h_note,          nullptr },
        { "/dl",        HTTP_GET,  h_download,      nullptr },
        { "/del",       HTTP_POST, h_delete,        nullptr },
        { "/settings",  HTTP_GET,  h_settings_get,  nullptr },
        { "/settings",  HTTP_POST, h_settings_post, nullptr },
        { "/sync",      HTTP_POST, h_sync,          nullptr },
    };
    for (auto &u : routes) httpd_register_uri_handler(s_server, &u);
    ESP_LOGI(TAG, "dashboard up");
}

void webserver_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = nullptr; }
}

bool webserver_is_running(void) { return s_server != nullptr; }
