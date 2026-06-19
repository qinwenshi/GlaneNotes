// sync.cpp — scan SD notes and transcribe any lacking a .txt transcript.
#include "sync.h"
#include "glane_config.h"
#include "transcribe.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "sync";

void notes_ensure_dir(void)
{
    struct stat st;
    if (stat(NOTES_DIR, &st) != 0) {
        mkdir(NOTES_DIR, 0775);
    }
}

static bool ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return false;
    return strcasecmp(s + ls - lf, suf) == 0;
}

static void path_for(char *buf, size_t n, const char *id, const char *ext)
{
    snprintf(buf, n, "%s/%s%s", NOTES_DIR, id, ext);
}

static bool text_exists(const char *id)
{
    char p[160];
    path_for(p, sizeof(p), id, ".txt");
    struct stat st;
    return stat(p, &st) == 0;
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(((const note_info_t *)b)->id, ((const note_info_t *)a)->id);
}

int notes_scan(note_info_t *out, int max)
{
    DIR *d = opendir(NOTES_DIR);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (!ends_with(e->d_name, ".wav")) continue;
        note_info_t *ni = &out[n];
        // strip ".wav"
        size_t len = strlen(e->d_name) - 4;
        if (len >= sizeof(ni->id)) len = sizeof(ni->id) - 1;
        memcpy(ni->id, e->d_name, len);
        ni->id[len] = '\0';

        char p[160];
        path_for(p, sizeof(p), ni->id, ".wav");
        struct stat st;
        ni->wav_bytes = (stat(p, &st) == 0) ? (uint32_t)st.st_size : 0;
        ni->mtime     = (stat(p, &st) == 0) ? st.st_mtime : 0;
        ni->has_text  = text_exists(ni->id);
        n++;
    }
    closedir(d);

    qsort(out, n, sizeof(note_info_t), cmp_desc);
    return n;
}

int notes_pending_count(void)
{
    static note_info_t list[MAX_NOTES];
    int n = notes_scan(list, MAX_NOTES);
    int pending = 0;
    for (int i = 0; i < n; i++) if (!list[i].has_text) pending++;
    return pending;
}

char *notes_read_text(const char *id)
{
    char p[160];
    path_for(p, sizeof(p), id, ".txt");
    FILE *f = fopen(p, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return nullptr; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int sync_run(sync_progress_cb_t cb)
{
    static note_info_t list[MAX_NOTES];
    int n = notes_scan(list, MAX_NOTES);

    int total = 0;
    for (int i = 0; i < n; i++) if (!list[i].has_text) total++;
    if (total == 0) {
        if (cb) cb(0, 0);
        return 0;
    }

    int done = 0, ok = 0;
    if (cb) cb(done, total);

    for (int i = 0; i < n; i++) {
        if (list[i].has_text) continue;

        char wav[160], txt[160];
        path_for(wav, sizeof(wav), list[i].id, ".wav");
        path_for(txt, sizeof(txt), list[i].id, ".txt");

        char *text = nullptr;
        tr_result_t r = transcribe_wav_file(wav, &text);
        if (r == TR_OK && text) {
            FILE *f = fopen(txt, "wb");
            if (f) {
                fwrite(text, 1, strlen(text), f);
                fclose(f);
                ok++;
                ESP_LOGI(TAG, "%s -> %u chars", list[i].id, (unsigned)strlen(text));
            }
            free(text);
        } else if (r == TR_TOO_LARGE) {
            // Mark as handled so we don't retry forever: write a stub note.
            FILE *f = fopen(txt, "wb");
            if (f) {
                const char *m = "[audio too long for auto-transcription]";
                fwrite(m, 1, strlen(m), f);
                fclose(f);
            }
            ESP_LOGW(TAG, "%s too large, stubbed", list[i].id);
        } else {
            ESP_LOGW(TAG, "%s transcription failed (%d)", list[i].id, r);
            // Leave it pending for a future sync.
        }

        done++;
        if (cb) cb(done, total);
    }
    ESP_LOGI(TAG, "sync done: %d/%d transcribed", ok, total);
    return ok;
}
