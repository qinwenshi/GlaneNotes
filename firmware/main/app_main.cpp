// app_main.cpp — Glane Notes firmware entry point & state machine.
//
// Behaviour (per the Glane Notes design — capture now, process later):
//   • BOOT short-press toggles recording. Audio is captured to the SD card as
//     a 16 kHz mono WAV. Nothing leaves the device during capture.
//   • BOOT long-press (or the web "Sync now" button) connects Wi-Fi and uploads
//     each note lacking a transcript to Aliyun for file-based STT; the returned
//     text is written back next to the audio on the SD card.
//   • A local web dashboard (when Wi-Fi is up) lists, plays, and downloads notes.
//   • After an idle period the device enters deep sleep; BOOT wakes it.
//
// Hard state gates (one job at a time) avoid SD / Wi-Fi / I2S contention.

#include "glane_config.h"
#include "settings.h"
#include "sdcard.h"
#include "buttons.h"
#include "recorder.h"
#include "tone.h"
#include "player.h"
#include "battery.h"
#include "timesync.h"
#include "ui.h"
#include "wifi_mgr.h"
#include "sync.h"
#include "webserver.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "glane";

#define PWR_LONG_MS 800   // PWR held this long counts as a long press

static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ── State ────────────────────────────────────────────────────────────────────
enum class State { IDLE, RECORDING, SYNCING, LIST, DETAIL, PLAYING, PROVISION };
static State            s_state          = State::IDLE;
static volatile bool    s_sync_requested = false;   // set by web handler
static volatile bool    s_wifi_connecting = false;  // boot-time bg connect in flight
static volatile bool    s_wifi_evt        = false;  // bg connect just succeeded
static uint32_t         s_last_activity  = 0;
static uint32_t         s_rec_ui_tick    = 0;
static char             s_rec_path[160]  = {0};

// ── Notes browser ────────────────────────────────────────────────────────────
static note_info_t      s_notes[MAX_NOTES];
static int              s_note_count = 0;
static int              s_sel = 0;       // selected absolute index
static int              s_top = 0;       // first visible row index
static const int        LIST_ROWS = 5;
static inline int       sel_number(void) { return s_note_count - s_sel; }

// ── Power rails ──────────────────────────────────────────────────────────────
static void power_init(void)
{
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << VBAT_LATCH) | (1ULL << EPD_PWR)
                     | (1ULL << AUDIO_PWR)  | (1ULL << PA_PIN);
    gpio_config(&out);
    gpio_set_level((gpio_num_t)VBAT_LATCH, 1);   // hold power on
    gpio_set_level((gpio_num_t)EPD_PWR,    0);   // e-paper ON  (active LOW)
    gpio_set_level((gpio_num_t)AUDIO_PWR,  0);   // audio ON    (active LOW)
    gpio_set_level((gpio_num_t)PA_PIN,     0);   // amp OFF (no playback needed)
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ── Codec bring-up (shared I2C + ES8311) ─────────────────────────────────────
extern "C" bool codec_init(int sda, int scl, uint32_t freq_hz);

// ── UI refresh helpers ───────────────────────────────────────────────────────
static int count_notes(void)
{
    static note_info_t list[MAX_NOTES];
    return notes_scan(list, MAX_NOTES);
}

static void show_idle(void)
{
    ui_show_idle(count_notes(), wifi_mgr_is_connected() ? 1 : 0, wifi_mgr_ip(),
                 battery_percent());
}

// ── Sync flow ────────────────────────────────────────────────────────────────
static void on_web_sync_request(void) { s_sync_requested = true; }

// Best-effort Wi-Fi connect at boot, off the main path. Capture works fully
// offline, so a failed/slow connection must never block recording. On success
// we bring up the dashboard + clock and let the main loop refresh the screen.
static void boot_wifi_task(void *)
{
    s_wifi_connecting = true;
    bool ok = wifi_mgr_connect(settings_get()->wifi_ssid,
                               settings_get()->wifi_pass, 12000);
    if (ok) {
        webserver_start(on_web_sync_request);
        timesync_start();
        s_wifi_evt = true;   // main loop refreshes idle to show "WiFi: ON"
    }
    s_wifi_connecting = false;
    vTaskDelete(nullptr);
}

#define AP_SSID "GlaneNotes-Setup"   // open SoftAP for first-time Wi-Fi setup

// Host the Wi-Fi/API-key dashboard over the device's own access point so it can
// be configured with no prior network (solves the STA chicken-and-egg problem).
static void start_provisioning(void)
{
    s_state = State::PROVISION;
    const char *ip = wifi_mgr_start_ap(AP_SSID, nullptr);   // open network
    if (!webserver_is_running()) webserver_start(on_web_sync_request);
    settings_take_dirty();                                  // clear stale flag
    char url[40];
    snprintf(url, sizeof(url), "http://%s", ip);
    ui_show_provision(AP_SSID, url);
}

// Leave provisioning without saving — tear down the AP and return home.
static void cancel_provisioning(void)
{
    wifi_mgr_stop_ap();
    webserver_stop();
    s_state = State::IDLE;
    s_last_activity = millis();
    show_idle();
}

static void sync_progress(int done, int total) { ui_show_syncing(done, total); }

static void do_sync(void)
{
    s_state = State::SYNCING;

    if (!settings_has_wifi()) {
        start_provisioning();   // host setup AP instead of a dead end
        return;
    }

    if (!wifi_mgr_is_connected()) {
        ui_show_message("CONNECTING", settings_get()->wifi_ssid);
        if (s_wifi_connecting) {
            // A boot-time connect is already in flight — wait for it rather than
            // starting a second, racing esp_wifi_connect().
            int waited = 0;
            while (s_wifi_connecting && waited < 15000) {
                vTaskDelay(pdMS_TO_TICKS(200));
                waited += 200;
            }
        } else {
            wifi_mgr_connect(settings_get()->wifi_ssid, settings_get()->wifi_pass, 15000);
        }
    }

    if (!wifi_mgr_is_connected()) {
        ui_show_message("WIFI FAILED", "Working offline");
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_state = State::IDLE;
        show_idle();
        return;
    }

    // Bring up the dashboard now that Wi-Fi is up.
    if (!webserver_is_running()) webserver_start(on_web_sync_request);
    timesync_start();   // get real time for note timestamps

    if (!settings_has_api_key()) {
        char ip[40];
        snprintf(ip, sizeof(ip), "http://%s", wifi_mgr_ip());
        ui_show_message("SET API KEY", ip);
        vTaskDelay(pdMS_TO_TICKS(2500));
        s_state = State::IDLE;
        show_idle();
        return;
    }

    int pending = notes_pending_count();
    if (pending == 0) {
        ui_show_message("ALL SYNCED", wifi_mgr_ip());
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ui_show_syncing(0, pending);
        int ok = sync_run(sync_progress);
        char l2[40];
        snprintf(l2, sizeof(l2), "%d transcribed", ok);
        ui_show_message("SYNC DONE", l2);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    s_state = State::IDLE;
    s_last_activity = millis();
    show_idle();
}

// ── Recording flow ───────────────────────────────────────────────────────────
static void start_recording(void)
{
    notes_ensure_dir();
    // Persistent NVS counter — unique across deep sleep and power cycles
    // (boot-relative uptime would reset and collide after a wake reboot).
    uint32_t id = settings_next_note_seq();
    snprintf(s_rec_path, sizeof(s_rec_path), "%s/note-%010u.wav", NOTES_DIR, (unsigned)id);

    tone_play_start();   // audible cue before I2S0 is handed to the recorder

    if (!recorder_start(s_rec_path)) {
        ui_show_message("REC FAILED", "Check SD card");
        vTaskDelay(pdMS_TO_TICKS(1500));
        show_idle();
        return;
    }
    s_state = State::RECORDING;
    s_rec_ui_tick = millis();
    ui_show_recording(0);
}

static void stop_recording(void)
{
    // Hand the desired file mtime to the recorder so the background finalize can
    // stamp it after the WAV is closed (FAT mtime is otherwise 1980 until SNTP).
    if (timesync_is_valid())
        recorder_set_save_time((int64_t)time(nullptr));
    recorder_stop();     // returns fast; SD flush/close finishes in the background
    tone_play_stop();    // audible cue — I2S0 is freed synchronously by recorder_stop()
    s_state = State::IDLE;
    s_last_activity = millis();
    ui_show_message("SAVED", "Note stored on SD");
    vTaskDelay(pdMS_TO_TICKS(1000));
    show_idle();
}

// ── Deep sleep ───────────────────────────────────────────────────────────────
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "entering deep sleep");
    if (player_is_active()) player_stop();
    if (recorder_is_active()) recorder_stop();
    recorder_wait_finalized(8000);   // ensure the WAV is fully written before the SD rail drops
    webserver_stop();
    wifi_mgr_disconnect();
    ui_show_message("SLEEPING", "Press to wake");

    gpio_set_level((gpio_num_t)AUDIO_PWR, 1);   // audio rail off
    gpio_set_level((gpio_num_t)EPD_PWR,   1);   // e-paper rail off

    esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

// ── Notes browser navigation ─────────────────────────────────────────────────
static void go_idle(void)
{
    s_state = State::IDLE;
    s_last_activity = millis();
    show_idle();
}

static void open_list(void)
{
    s_note_count = notes_scan(s_notes, MAX_NOTES);
    s_sel = 0;
    s_top = 0;
    s_state = State::LIST;
    ui_show_list(s_notes, s_note_count, s_sel, s_top);
}

static void list_move_down(void)
{
    if (s_note_count == 0) return;
    s_sel = (s_sel + 1) % s_note_count;
    if (s_sel == 0)                 s_top = 0;                  // wrapped to top
    else if (s_sel < s_top)         s_top = s_sel;
    else if (s_sel >= s_top + LIST_ROWS) s_top = s_sel - (LIST_ROWS - 1);
    ui_show_list(s_notes, s_note_count, s_sel, s_top);
}

static void open_detail(void)
{
    if (s_note_count == 0) { go_idle(); return; }
    s_state = State::DETAIL;
    ui_show_detail(&s_notes[s_sel], sel_number());
}

static void back_to_list(void)
{
    s_state = State::LIST;
    ui_show_list(s_notes, s_note_count, s_sel, s_top);
}

static void play_current(void)
{
    if (s_note_count == 0) return;
    char path[200];
    snprintf(path, sizeof(path), "%s/%s.wav", NOTES_DIR, s_notes[s_sel].id);
    s_state = State::PLAYING;
    ui_show_playing(sel_number());
    if (!player_play(path)) {
        ui_show_message("PLAY FAILED", "Cannot open file");
        vTaskDelay(pdMS_TO_TICKS(1200));
        s_state = State::DETAIL;
        ui_show_detail(&s_notes[s_sel], sel_number());
    }
}

static void stop_playback(void)
{
    player_stop();
    s_state = State::DETAIL;
    ui_show_detail(&s_notes[s_sel], sel_number());
}

// ── Main loop ────────────────────────────────────────────────────────────────
static void main_task(void *)
{
    power_init();

    // NVS (settings + wifi).
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_init();

    ui_init();
    ui_show_message("GLANE NOTES", "starting...");

    if (!sdcard_mount(SD_CLK, SD_CMD, SD_D0)) {
        ui_show_message("NO SD CARD", "Insert & reboot");
    } else {
        notes_ensure_dir();
    }

    if (!codec_init(CODEC_SDA, CODEC_SCL, 100000)) {
        ESP_LOGE(TAG, "codec init failed");
    }
    recorder_init();
    player_init();
    battery_init();
    wifi_mgr_init();

    // Kick off a non-blocking Wi-Fi connect if configured. Recording stays
    // available immediately; the dashboard appears later only if it succeeds.
    if (settings_has_wifi()) {
        xTaskCreatePinnedToCore(boot_wifi_task, "boot_wifi", 4096, nullptr, 4, nullptr, 0);
    }

    buttons_init(BOOT_BTN, PWR_BTN);
    s_last_activity = millis();

    // First-time setup: no Wi-Fi yet → host the provisioning AP automatically.
    if (!settings_has_wifi()) start_provisioning();
    else                      show_idle();

    for (;;) {
        uint32_t held = 0;

        // ── BOOT ──────────────────────────────────────────────────────────
        if (buttons_boot_long_fired()) {
            s_last_activity = millis();
            switch (s_state) {
                case State::RECORDING: stop_recording(); break;
                case State::IDLE:      do_sync();        break;
                case State::LIST:
                case State::DETAIL:    go_idle();        break;  // back home
                default: break;
            }
        } else if (buttons_boot_fired(&held)) {
            s_last_activity = millis();
            switch (s_state) {
                case State::IDLE:      start_recording(); break;
                case State::RECORDING: stop_recording();  break;
                case State::LIST:      open_detail();     break;
                case State::DETAIL:    play_current();    break;
                case State::PLAYING:   stop_playback();   break;
                case State::PROVISION: cancel_provisioning(); break;
                default: break;
            }
        }

        // ── PWR (short vs long via held) ──────────────────────────────────
        if (buttons_pwr_fired(&held)) {
            s_last_activity = millis();
            bool lng = (held >= PWR_LONG_MS);
            switch (s_state) {
                case State::IDLE:      lng ? enter_deep_sleep() : open_list();      break;
                case State::RECORDING: stop_recording();                            break;
                case State::LIST:      lng ? enter_deep_sleep() : list_move_down();  break;
                case State::DETAIL:    back_to_list();                              break;
                case State::PLAYING:   stop_playback();                             break;
                case State::PROVISION: cancel_provisioning();                       break;
                default: break;
            }
        }

        // Background Wi-Fi connect finished — refresh the home screen so it
        // shows "WiFi: ON" and the IP without ever having blocked startup.
        if (s_wifi_evt) {
            s_wifi_evt = false;
            if (s_state == State::IDLE) show_idle();
        }

        // Web-requested sync (only honoured when idle).
        if (s_sync_requested) {
            s_sync_requested = false;
            if (s_state == State::IDLE) {
                s_last_activity = millis();
                do_sync();
            }
        }

        // Recording UI: update the elapsed clock once per second (partial refresh).
        if (s_state == State::RECORDING) {
            if (!recorder_is_active()) {
                stop_recording();   // recorder hit its max-length cap
            } else if (millis() - s_rec_ui_tick >= 1000) {
                s_rec_ui_tick = millis();
                ui_show_recording(recorder_elapsed_ms() / 1000);
            }
        }

        // Playback finished on its own → return to the note detail.
        if (s_state == State::PLAYING && !player_is_active()) {
            s_state = State::DETAIL;
            ui_show_detail(&s_notes[s_sel], sel_number());
        }

        // Provisioning: once the user saves Wi-Fi from the web form, reboot so
        // the device comes back up and auto-connects as a station.
        if (s_state == State::PROVISION && settings_take_dirty() &&
            settings_has_wifi()) {
            ui_show_message("SAVED", "Restarting...");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }

        // Idle deep-sleep timer (only from the home screen).
        if (s_state == State::IDLE &&
            millis() - s_last_activity > IDLE_SLEEP_TIMEOUT_MS) {
            enter_deep_sleep();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void)
{
    // Run on its own task with a generous stack (e-ink, SD, TLS all need room).
    xTaskCreatePinnedToCore(main_task, "glane_main", 12288, nullptr, 5, nullptr, 1);
}
