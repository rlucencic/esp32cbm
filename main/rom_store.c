#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/roms"
#define SLOT                16384

static const char *TAG = "rom";

static const struct {
    const char *name;
    uint8_t slot;
    uint32_t size;
} roms[ROM_COUNT] = {
    { "c64basic.rom", 0, 8192 }, { "c64kernal.rom", 1, 8192 }, { "c64chargen.rom", 2, 4096 },
    { "1541.rom", 3, 16384 }, { "c128basic.rom", 4, 32768 }, { "c128kernal.rom", 6, 16384 },
    { "c128chargen.rom", 7, 8192 }, { "c128c64.rom", 8, 16384 }, { "1571.rom", 9, 32768 },
};

const char *rom_store_name(rom_id_t id)
{
    return id < ROM_COUNT ? roms[id].name : "";
}

uint32_t rom_store_size(rom_id_t id)
{
    return id < ROM_COUNT ? roms[id].size : 0;
}

/* Not erased flash, not zeros: something at the start of most 4 KB blocks
 * (the 1571's ROM has one that starts with $FF bytes)
 */
static bool plausible(const uint8_t *p, uint32_t size)
{
    int blank = 0, blocks = 0;
    for (uint32_t off = 0; off < size; off += 4096, blocks++) {
        bool ff = true, zero = true;
        for (int i = 0; i < 64; i++) {
            ff &= p[off + i] == 0xFF;
            zero &= p[off + i] == 0x00;
        }
        blank += ff || zero;
    }
    return blank * 4 <= blocks;
}

/* Through a mapping, not esp_partition_read(): that turns the caches off
 * while it reads, which stalls the LCD's refill (picture artefacts)
 */
static bool from_partition(uint32_t offset, uint32_t size, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           "cbm_roms");
    if (part == NULL || offset + size > part->size) {
        return false;
    }
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, offset, size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, size);
    esp_partition_munmap(handle);
    return true;
}

/* roms/<name> on the SD card (any case) */
static long from_sd(const char *name, uint8_t *buf, uint32_t max)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        for (char *p = path + strlen(ROM_FOLDER) + 1; *p; p++) {
            *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        }
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        return -1;
    }
    long n = fread(buf, 1, max, f);
    fclose(f);
    if (n > 0) {
        ESP_LOGI(TAG, "%s from the SD card", name);
    }
    return n;
}

bool rom_store_load(rom_id_t id, uint8_t *buf)
{
    if (id >= ROM_COUNT) {
        return false;
    }
    uint32_t size = rom_store_size(id);
    if (from_partition(roms[id].slot * SLOT, size, buf) && plausible(buf, size)) {
        return true;
    }
    return from_sd(roms[id].name, buf, size) == (long)size && plausible(buf, size);
}
