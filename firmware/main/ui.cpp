// ui.cpp — e-ink status screens drawn with a 5x7 bitmap font.
//
// Refresh strategy (optimised for minimal flashing):
//   • Every screen update — including switching between different screens
//     (idle→list→detail→playing) — uses the fast, NON-flashing partial refresh.
//   • A full (flashing) refresh is only used occasionally, once every
//     FULL_EVERY partial updates, purely to clear accumulated ghosting.
//   • That periodic full refresh flashes the panel exactly ONCE: we paint with
//     EPD_Display() and then re-seed the partial base into RAM *silently*
//     (EPD_WriteFrameToRAMSilent) instead of EPD_DisplayPartBaseImage(), which
//     would trigger a second panel turn-on (the old double-flash).
#include "ui.h"
#include "glane_config.h"
#include "font5x7.h"
#include "dog_sprites.h"
#include "timesync.h"
#include "epaper_driver_bsp.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";
static epaper_driver_display *s_epd = nullptr;

// Scratch buffer used to re-seed the partial-refresh base without flashing.
static uint8_t s_basebuf[(EPD_W * EPD_H) / 8];

// Which logical screen is currently shown (informational only — no longer forces
// a full refresh, so moving between screens stays flash-free).
enum ScreenKind {
    SCR_NONE = 0, SCR_IDLE, SCR_REC, SCR_MSG, SCR_SYNC,
    SCR_LIST, SCR_DETAIL, SCR_PLAY, SCR_TAG, SCR_SLEEP,
};
static ScreenKind s_scr = SCR_NONE;
static int        s_partial_count = 0;
static const int  FULL_EVERY = 60;   // de-ghost full refresh cadence (rarely hit)

// Re-seed both RAM planes with the current frame, without any panel turn-on.
static void seed_partial_base(void)
{
    s_epd->EPD_GetBuffer(s_basebuf);
    s_epd->EPD_WriteFrameToRAMSilent(s_basebuf);
}

// Push the current buffer to the panel, choosing partial vs full refresh.
static void commit(ScreenKind kind)
{
    if (!s_epd) return;
    bool force_full = (s_partial_count >= FULL_EVERY);   // ghosting cleanup only
    s_scr = kind;
    if (force_full) {
        s_epd->EPD_Init();          // full-LUT init
        s_epd->EPD_Display();       // single full (flashing) refresh
        s_epd->EPD_Init_Partial();  // re-enter partial mode (HW reset clears RAM)
        seed_partial_base();        // restore base into RAM — no extra flash
        s_partial_count = 0;
    } else {
        s_epd->EPD_DisplayPart();   // fast, no-flash partial refresh
        s_partial_count++;
    }
}

// Push the current buffer with a clean FULL (flashing) refresh. Used for the
// deep-sleep screen, which stays latched on the panel after the e-paper rail is
// cut — a partial refresh could leave ghosting or a half-formed image frozen on
// the powered-off display. No need to re-enter partial mode: deep sleep follows.
static void commit_full(ScreenKind kind)
{
    if (!s_epd) return;
    s_scr = kind;
    s_epd->EPD_Init();
    s_epd->EPD_Display();
    s_partial_count = 0;
}

// ── Primitive drawing into the EPD buffer ────────────────────────────────────
static void px(int x, int y, bool black)
{
    if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) return;
    s_epd->EPD_DrawColorPixel((uint16_t)x, (uint16_t)y,
                              black ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
}

// Draw one glyph at (x,y) with integer scale. Returns advance in pixels.
static int draw_char(int x, int y, char c, int scale)
{
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t *g = FONT5X7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sx = 0; sx < scale; sx++)
                    for (int sy = 0; sy < scale; sy++)
                        px(x + col * scale + sx, y + row * scale + sy, true);
            }
        }
    }
    return 6 * scale; // 5 cols + 1 spacing
}

static void draw_text(int x, int y, const char *s, int scale)
{
    while (*s) {
        x += draw_char(x, y, *s++, scale);
        if (x > EPD_W) break;
    }
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static void draw_text_centered(int y, const char *s, int scale)
{
    int w = text_width(s, scale);
    draw_text((EPD_W - w) / 2, y, s, scale);
}

static void hline(int y, int x0, int x1)
{
    for (int x = x0; x <= x1; x++) px(x, y, true);
}

static void vline(int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) px(x, y, true);
}

static void rect(int x0, int y0, int x1, int y1)
{
    hline(y0, x0, x1); hline(y1, x0, x1);
    vline(x0, y0, y1); vline(x1, y0, y1);
}

static void fill_rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) px(x, y, true);
}

// Draw a 1-bit LVGL-I1 sprite frame at (ox,oy). The 8-byte palette is skipped;
// pixels are packed MSB-first, one bit per pixel, and bit==1 means black.
static void draw_i1_sprite(const uint8_t *data, int w, int h, int stride,
                           int ox, int oy)
{
    const uint8_t *bits = data + 8;   // skip the 2-colour palette header
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bits + y * stride;
        for (int x = 0; x < w; x++) {
            if ((row[x >> 3] >> (7 - (x & 7))) & 1) px(ox + x, oy + y, true);
        }
    }
}

// ── Home-screen dog layout (right half of the 200x200 panel) ─────────────────
static const int DOG_OX = 104;
static const int DOG_OY = 44;

// Repaint just the dog region white, then stamp one running frame into it.
static void draw_idle_dog(int frame)
{
    if (frame < 0) frame = 0;
    frame %= DOG_RUNNING_FRAMES;
    for (int y = DOG_OY; y < DOG_OY + DOG_SPRITE_H; y++)
        for (int x = DOG_OX; x < DOG_OX + DOG_SPRITE_W; x++) px(x, y, false);
    draw_i1_sprite(dog_running_frames[frame], DOG_SPRITE_W, DOG_SPRITE_H,
                   DOG_SPRITE_STRIDE, DOG_OX, DOG_OY);
}

// Small battery icon at top-left (x,y): an outline, a positive-terminal nub, and
// a fill proportional to `pct` (0..100). pct < 0 draws an empty "?" battery.
static void battery_icon(int x, int y, int pct)
{
    const int w = 26, h = 8;           // body size (shorter)
    rect(x, y, x + w, y + h);          // outline
    fill_rect(x + w + 1, y + h / 2 - 1, x + w + 2, y + h / 2 + 1);  // terminal nub
    if (pct < 0) {
        draw_char(x + w / 2 - 3, y + 2, '?', 1);
        return;
    }
    int inner = w - 4;                 // fillable width
    int fw = inner * pct / 100;
    if (fw > 0) fill_rect(x + 2, y + 2, x + 2 + fw, y + h - 2);
}

// Small solid right-pointing triangle cursor at (x,y), height h.
static void cursor(int x, int y, int h)
{
    for (int row = 0; row < h; row++) {
        int w = (row <= h / 2) ? row : (h - 1 - row);
        for (int c = 0; c <= w; c++) px(x + c, y + row, true);
    }
}

// Right-align text ending at x_right.
static void draw_text_right(int x_right, int y, const char *s, int scale)
{
    draw_text(x_right - text_width(s, scale), y, s, scale);
}

// ── Public ───────────────────────────────────────────────────────────────────
void ui_init(void)
{
    if (s_epd) return;
    custom_lcd_spi_t cfg = {};
    cfg.cs   = EPD_CS;
    cfg.dc   = EPD_DC;
    cfg.rst  = EPD_RST;
    cfg.busy = EPD_BUSY;
    cfg.mosi = EPD_MOSI;
    cfg.scl  = EPD_SCK;
    cfg.spi_host   = 1; // SPI2_HOST
    cfg.buffer_len = (EPD_W * EPD_H) / 8;
    s_epd = new epaper_driver_display(EPD_W, EPD_H, cfg);
    s_epd->EPD_Init();
    s_epd->EPD_Clear();
    s_epd->EPD_Display();                // initial clean full refresh (white) — one flash
    s_epd->EPD_Init_Partial();           // enter partial-refresh mode (HW reset clears RAM)
    seed_partial_base();                 // seed base into RAM silently (no 2nd flash)
    s_scr = SCR_NONE;
    s_partial_count = 0;
    ESP_LOGI(TAG, "e-ink ready");
}

void ui_show_idle(int note_count, int wifi_connected, const char *ip, int battery_pct)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();

    // Battery indicator, top-RIGHT, with the percentage text to its left.
    battery_icon(162, 6, battery_pct);
    if (battery_pct >= 0) {
        char b[16];
        snprintf(b, sizeof(b), "%d%%", battery_pct);
        draw_text_right(157, 7, b, 1);
    }

    draw_text_centered(24, "GLANE NOTES", 2);

    // Left column: larger status text. Right half: the dog.
    char buf[48];
    snprintf(buf, sizeof(buf), "Notes:%d", note_count);
    draw_text(6, 52, buf, 2);
    draw_text(6, 80, wifi_connected ? "WiFi:ON" : "WiFi:--", 2);

    draw_idle_dog(0);

    // IP shown larger, centered just below the dog.
    if (wifi_connected && ip && ip[0]) draw_text_centered(150, ip, 2);

    draw_text_centered(180, "PRESS:REC  HOLD:SYNC", 1);
    commit(SCR_IDLE);
}

int ui_idle_dog_frames(void) { return DOG_RUNNING_FRAMES; }

// Advance the home-screen dog animation by one frame (fast partial refresh of
// just the dog region). The rest of the idle screen is left untouched.
void ui_idle_dog_anim(int frame)
{
    if (!s_epd || s_scr != SCR_IDLE) return;
    draw_idle_dog(frame);
    s_epd->EPD_DisplayPart();
    s_partial_count++;
}

// Sleep screen: the resting dog + "Zzz". Retained on the panel during deep sleep.
void ui_show_sleeping(void)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    draw_text_centered(14, "SLEEPING", 2);

    int ox = (EPD_W - DOG_SPRITE_W) / 2;
    int oy = 44;
    draw_i1_sprite(dog_sleep_frame, DOG_SPRITE_W, DOG_SPRITE_H,
                   DOG_SPRITE_STRIDE, ox, oy);

    // A little "Zzz" floating above the dog's head (top-right of the sprite).
    draw_text(ox + DOG_SPRITE_W - 10, oy - 2, "z", 1);
    draw_text(ox + DOG_SPRITE_W - 2,  oy - 8, "z", 2);
    draw_text(ox + DOG_SPRITE_W + 12, oy - 16, "Z", 3);

    draw_text_centered(170, "Press to wake", 1);
    commit_full(SCR_SLEEP);
}

void ui_show_recording(uint32_t elapsed_sec)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    draw_text_centered(30, "RECORDING", 3);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u",
             (unsigned)(elapsed_sec / 60), (unsigned)(elapsed_sec % 60));
    draw_text_centered(86, buf, 5);

    hline(150, 20, EPD_W - 20);
    draw_text_centered(168, "PRESS TO STOP", 1);
    commit(SCR_REC);
}

void ui_show_message(const char *line1, const char *line2)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    if (line1) draw_text_centered(70, line1, 2);
    if (line2) draw_text_centered(110, line2, 1);
    commit(SCR_MSG);
}

void ui_show_provision(const char *ssid, const char *url)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    draw_text_centered(14, "WIFI SETUP", 2);
    hline(40, 16, EPD_W - 16);

    draw_text(16, 56,  "1. Join Wi-Fi:", 1);
    if (ssid) draw_text_centered(74, ssid, 2);

    draw_text(16, 108, "2. Open in browser:", 1);
    if (url) draw_text_centered(126, url, 1);

    hline(160, 16, EPD_W - 16);
    draw_text_centered(170, "Set Wi-Fi + API key", 1);
    draw_text_centered(184, "PRESS to cancel", 1);
    commit(SCR_TAG);   // distinct kind; refresh strategy is partial regardless
}

void ui_show_syncing(int done, int total)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    draw_text_centered(40, "SYNCING", 3);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", done, total);
    draw_text_centered(100, buf, 3);

    // progress bar
    int x0 = 20, x1 = EPD_W - 20, y = 150;
    for (int x = x0; x <= x1; x++) { px(x, y, true); px(x, y + 10, true); }
    px(x0, y + 5, true); px(x1, y + 5, true);
    if (total > 0) {
        int fill = x0 + (x1 - x0) * done / total;
        for (int x = x0; x <= fill; x++)
            for (int yy = y + 1; yy < y + 10; yy++) px(x, yy, true);
    }
    commit(SCR_SYNC);
}

// ── Notes browser ────────────────────────────────────────────────────────────
void ui_show_list(const note_info_t *items, int count, int selected, int top)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();

    draw_text(6, 5, "notes", 1);
    char hb[24];
    snprintf(hb, sizeof(hb), "%d notes", count);
    draw_text_right(194, 5, hb, 1);
    hline(18, 4, 196);

    if (count == 0) {
        draw_text_centered(90, "NO NOTES", 2);
        commit(SCR_LIST);
        return;
    }

    const int rowh = 34, y0 = 24, rows = 5;
    for (int r = 0; r < rows; r++) {
        int idx = top + r;
        if (idx >= count) break;
        int ry = y0 + r * rowh;
        bool sel = (idx == selected);
        if (sel) {
            rect(14, ry, 196, ry + rowh - 4);
            cursor(3, ry + 8, 14);
        }
        int number = count - idx;   // newest gets the highest number
        char tb[16];
        timesync_format(items[idx].mtime, tb, sizeof(tb));
        char l1[40];
        snprintf(l1, sizeof(l1), "#%03d  %s", number, tb);
        draw_text(20, ry + 4, l1, 1);

        unsigned long sec = (unsigned long)(items[idx].wav_bytes / (16000UL * 2));
        char l2[40];
        snprintf(l2, sizeof(l2), "%lus  %s", sec, items[idx].has_text ? "[TXT]" : "[...]");
        draw_text(20, ry + 18, l2, 1);
    }
    commit(SCR_LIST);
}

void ui_show_detail(const note_info_t *item, int number)
{
    if (!s_epd || !item) return;
    s_epd->EPD_Clear();

    char t[24];
    snprintf(t, sizeof(t), "NOTE #%03d", number);
    draw_text_centered(10, t, 2);
    hline(34, 10, 190);

    char tb[16];
    timesync_format(item->mtime, tb, sizeof(tb));
    char b[48];
    snprintf(b, sizeof(b), "Time:   %s", tb);                       draw_text(16, 48, b, 1);
    unsigned long sec = (unsigned long)(item->wav_bytes / (16000UL * 2));
    snprintf(b, sizeof(b), "Length: %lus", sec);                    draw_text(16, 68, b, 1);
    snprintf(b, sizeof(b), "Size:   %luKB", (unsigned long)(item->wav_bytes / 1024)); draw_text(16, 88, b, 1);
    draw_text(16, 108, item->has_text ? "Transcript: yes" : "Transcript: no", 1);

    hline(150, 10, 190);
    draw_text_centered(160, "BOOT: PLAY", 1);
    draw_text_centered(176, "PWR: BACK", 1);
    commit(SCR_DETAIL);
}

void ui_show_playing(int number)
{
    if (!s_epd) return;
    s_epd->EPD_Clear();
    draw_text_centered(40, "PLAYING", 3);
    char t[24];
    snprintf(t, sizeof(t), "NOTE #%03d", number);
    draw_text_centered(96, t, 2);
    hline(150, 20, 180);
    draw_text_centered(165, "PRESS TO STOP", 1);
    commit(SCR_PLAY);
}
