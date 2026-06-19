// sdcard.cpp — SD card via SDMMC (1-bit) + VFS FAT (ESP-IDF v5.x)
#include "sdcard.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card = nullptr;

bool sdcard_mount(int clk, int cmd, int d0)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // 40 MHz

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = (gpio_num_t)clk;
    slot.cmd   = (gpio_num_t)cmd;
    slot.d0    = (gpio_num_t)d0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files              = 8;
    mount_cfg.allocation_unit_size   = 16 * 1024;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot,
                                             &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

void sdcard_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_card = nullptr;
    }
}

bool sdcard_is_mounted(void)
{
    return s_card != nullptr;
}

uint64_t sdcard_total_bytes(void)
{
    if (!s_card) return 0;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) return 0;
    return total;
}

uint64_t sdcard_free_bytes(void)
{
    if (!s_card) return 0;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) != ESP_OK) return 0;
    return freeb;
}
