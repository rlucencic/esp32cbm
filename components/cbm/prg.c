/*
 * Programs straight into memory: .PRG (load address, then the bytes) and
 * .T64 (a tape archive: the first program in it), as LOAD"name",8,1 would
 * put them; a BASIC program (loaded at the start of BASIC) also gets its
 * end pointers set, and RUN typed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cbm_internal.h"

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

bool cbm_program_load(const char *path, bool run)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 2 && size <= 1024 * 1024 ? cbm_malloc(size) : NULL;
    bool ok = file && fread(file, 1, size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(file);
        return false;
    }
    uint16_t start;
    const uint8_t *data;
    long len;
    if (has_ext(path, ".t64")) {
        /* header 64 bytes, then 32-byte entries: type, file type, start, end, 0, offset */
        if (size < 96 || memcmp(file, "C64", 3) != 0) {
            free(file);
            return false;
        }
        const uint8_t *e = file + 64;
        start = e[2] | (e[3] << 8);
        uint16_t end = e[4] | (e[5] << 8);
        uint32_t off = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        len = end > start ? end - start : 0;
        if (off >= (uint32_t)size) {
            free(file);
            return false;
        }
        if (off + len > (uint32_t)size || len == 0) {
            len = size - off;           /* many T64s get the end address wrong */
        }
        data = file + off;
    } else {
        start = file[0] | (file[1] << 8);
        data = file + 2;
        len = size - 2;
    }
    if (start + len > 0x10000) {
        len = 0x10000 - start;
    }
    memcpy(cbm.cfg.ram + start, data, len);
    free(file);
    uint16_t end = start + len;
    uint16_t basic = cbm.cfg.model == CBM_MODEL_C128 ? 0x1C01 : 0x0801;
    if (start == basic) {
        /* VARTAB, ARYTAB, STREND: the program's end */
        uint8_t *ram = cbm.cfg.ram;
        int zp = cbm.cfg.model == CBM_MODEL_C128 ? 0x1210 : 0x2D;
        for (int i = 0; i < 3; i++) {
            ram[zp + i * 2] = end;
            ram[zp + i * 2 + 1] = end >> 8;
        }
        if (cbm.cfg.model == CBM_MODEL_C64) {
            ram[0xAE] = end;
            ram[0xAF] = end >> 8;
        }
        if (run) {
            cbm_type("run\n");
        }
    }
    CBM_LOG("%s at $%04X-$%04X", path, start, end);
    return true;
}
