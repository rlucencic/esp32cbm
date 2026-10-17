/*
 * Cartridges (.CRT): the expansion port's ROML ($8000) and ROMH ($A000, or
 * $E000 in Ultimax mode), the EXROM and GAME lines, and the I/O areas
 * $DE00/$DF00 where banking cartridges are switched:
 *
 *   0 normal (8 KB, 16 KB, Ultimax)  4 Simons' BASIC  5 Ocean  7 Fun Play
 *   8 Super Games  15 System 3 / C64GS  17 Dinamic  19 Magic Desk
 *   32 EasyFlash (its flash read-only, its 256 bytes of RAM at $DF00)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cbm_internal.h"

#define BANKS       64
#define MAX_CART    (1024 * 1024)

bool cart_exrom = true, cart_game = true;

static struct {
    char path[256];
    char info[48];
    uint16_t type;
    bool exrom0, game0;             /* the lines as the header gives them (true = high) */
    uint8_t *data;                  /* BANKS x (ROML 8 KB, ROMH 8 KB) */
    uint8_t bank;
    bool off;                       /* switched out (Magic Desk, Fun Play ...) */
    uint8_t ef_ram[256];
    bool present;
} c;

static void lines(bool exrom, bool game)
{
    cart_exrom = exrom;
    cart_game = game;
}

static uint8_t *bank_ptr(int bank, bool high)
{
    return c.data + (bank % BANKS) * 0x4000 + (high ? 0x2000 : 0);
}

const uint8_t *cart_roml(void)
{
    return c.present ? bank_ptr(c.bank, false) : NULL;
}

const uint8_t *cart_romh(void)
{
    if (!c.present) {
        return NULL;
    }
    if (c.type == 5 && c.info[0] && strstr(c.info, "512")) {
        return bank_ptr(c.bank, false);     /* 512 KB Ocean: the same bank at $A000 */
    }
    return bank_ptr(c.bank, true);
}

static void switched(void)
{
    mem_update();
}

uint8_t cart_io_read(uint16_t addr)
{
    if (!c.present) {
        return 0xFF;
    }
    switch (c.type) {
    case 4:                                 /* Simons' BASIC: reading $DE00 hides ROMH */
        if (addr == 0xDE00) {
            lines(false, true);
            switched();
        }
        break;
    case 17:                                /* Dinamic: reading $DE00+n selects bank n */
        if ((addr & 0xFF00) == 0xDE00) {
            c.bank = addr & 0x0F;
            switched();
        }
        break;
    case 32:
        if ((addr & 0xFF00) == 0xDF00) {
            return c.ef_ram[addr & 0xFF];
        }
        break;
    default:
        break;
    }
    return 0xFF;
}

void cart_io_write(uint16_t addr, uint8_t v)
{
    if (!c.present) {
        return;
    }
    switch (c.type) {
    case 4:
        if (addr == 0xDE00) {
            lines(false, false);
            switched();
        }
        break;
    case 5:                                 /* Ocean */
        if ((addr & 0xFF00) == 0xDE00) {
            c.bank = v & 0x3F;
            switched();
        }
        break;
    case 7:                                 /* Fun Play */
        if ((addr & 0xFF00) == 0xDE00) {
            if (v == 0x86) {
                lines(true, true);
            } else {
                c.bank = ((v >> 3) & 7) | ((v & 1) << 3);
                lines(false, true);
            }
            switched();
        }
        break;
    case 8:                                 /* Super Games */
        if ((addr & 0xFF00) == 0xDF00) {
            c.bank = v & 3;
            if (v & 4) {
                lines(true, true);
            } else {
                lines(false, false);
            }
            switched();
        }
        break;
    case 15:                                /* System 3: writing $DE00+n selects bank n */
        if ((addr & 0xFF00) == 0xDE00) {
            c.bank = addr & 0x3F;
            lines(false, true);
            switched();
        }
        break;
    case 19:                                /* Magic Desk */
        if ((addr & 0xFF00) == 0xDE00) {
            c.bank = v & 0x7F;
            lines(v & 0x80 ? true : false, true);
            switched();
        }
        break;
    case 32:                                /* EasyFlash */
        if (addr == 0xDE00) {
            c.bank = v & 0x3F;
            switched();
        } else if (addr == 0xDE02) {
            bool game = (v & 0x04) ? !(v & 0x01) : false;   /* mode bit: GAME from bit 0, else low */
            lines(!(v & 0x02), game);
            switched();
        } else if ((addr & 0xFF00) == 0xDF00) {
            c.ef_ram[addr & 0xFF] = v;
        }
        break;
    default:
        break;
    }
}

void cart_reset(void)
{
    c.bank = 0;
    c.off = false;
    if (c.present) {
        if (c.type == 32) {
            lines(true, false);             /* EasyFlash boots in Ultimax mode */
        } else {
            lines(c.exrom0, c.game0);
        }
    } else {
        lines(true, true);
    }
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static inline uint16_t be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

bool cbm_cart_insert(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 0x40 && size <= MAX_CART + 0x10000 ? cbm_malloc(size) : NULL;
    bool ok = file && fread(file, 1, size, f) == (size_t)size;
    fclose(f);
    if (!ok || memcmp(file, "C64 CARTRIDGE   ", 16) != 0) {
        free(file);
        return false;
    }
    if (c.data == NULL) {
        c.data = cbm_malloc(BANKS * 0x4000);
        if (c.data == NULL) {
            free(file);
            return false;
        }
    }
    memset(c.data, 0xFF, BANKS * 0x4000);
    uint16_t type = be16(file + 0x16);
    uint32_t pos = be32(file + 0x10);
    uint32_t total = 0;
    while (pos + 16 <= (uint32_t)size && memcmp(file + pos, "CHIP", 4) == 0) {
        uint32_t len = be32(file + pos + 4);
        uint16_t bank = be16(file + pos + 10), load = be16(file + pos + 12), n = be16(file + pos + 14);
        const uint8_t *d = file + pos + 16;
        if (pos + 16 + n > (uint32_t)size || len < 16) {
            break;
        }
        uint8_t *dst = bank_ptr(bank, load >= 0xA000);
        if (n > 0x2000 && load == 0x8000) {
            memcpy(dst, d, 0x2000);                         /* 16 KB chip: ROML and ROMH */
            memcpy(bank_ptr(bank, true), d + 0x2000, n - 0x2000 > 0x2000 ? 0x2000 : n - 0x2000);
        } else {
            memcpy(dst, d, n > 0x2000 ? 0x2000 : n);
        }
        total += n;
        pos += len;
    }
    c.type = type;
    c.exrom0 = file[0x18] != 0;
    c.game0 = file[0x19] != 0;
    static const char *const names[] = {
        [0] = "Normal", [4] = "Simons' BASIC", [5] = "Ocean", [7] = "Fun Play", [8] = "Super Games",
        [15] = "System 3", [17] = "Dinamic", [19] = "Magic Desk", [32] = "EasyFlash",
    };
    const char *name = type < sizeof(names) / sizeof(names[0]) && names[type] ? names[type] : NULL;
    if (name == NULL) {
        snprintf(c.info, sizeof(c.info), "type %u (not supported)", type);
    } else {
        snprintf(c.info, sizeof(c.info), "%s, %u KB", name, (unsigned)(total / 1024));
    }
    free(file);
    snprintf(c.path, sizeof(c.path), "%s", path);
    c.present = true;
    if (cbm.cfg.ram) {
        cbm_reset();                    /* the machine restarts with it */
    }
    return true;
}

void cbm_cart_eject(void)
{
    c.present = false;
    c.path[0] = 0;
    c.info[0] = 0;
    if (cbm.cfg.ram) {
        cbm_reset();
    }
}

const char *cbm_cart_path(void)
{
    return c.present ? c.path : NULL;
}

const char *cbm_cart_info(void)
{
    return c.present ? c.info : "";
}
