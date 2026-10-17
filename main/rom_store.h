#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The Commodore ROMs: from the cbm_roms flash partition (roms/<name> in the
 * project, written by idf.py flash into 16 KB slots), else from the SD
 * card's "roms" folder under the same names.
 */
typedef enum {
    ROM_C64_BASIC, ROM_C64_KERNAL, ROM_C64_CHARGEN, ROM_1541,
    ROM_C128_BASIC, ROM_C128_KERNAL, ROM_C128_CHARGEN, ROM_C128_C64, ROM_1571,
    ROM_COUNT
} rom_id_t;

const char *rom_store_name(rom_id_t id);    /* "c64basic.rom" */
uint32_t rom_store_size(rom_id_t id);

/* The whole ROM into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);
