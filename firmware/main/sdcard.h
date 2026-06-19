// sdcard.h — SD card via SDMMC (1-bit) + VFS FAT
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount SD card (1-bit SDMMC mode). Files accessible under /sdcard/.
bool sdcard_mount(int clk, int cmd, int d0);
void sdcard_unmount(void);
bool sdcard_is_mounted(void);

// Free / total bytes of the mounted card (0 if unmounted).
uint64_t sdcard_total_bytes(void);
uint64_t sdcard_free_bytes(void);

#ifdef __cplusplus
}
#endif
