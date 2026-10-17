/*
 * The C128's VDC (8563): 80 columns from its own 16 or 64 KB of RAM,
 * reached through two registers:
 *
 *   $D600 write: register number; read: status (bit 7 ready, 5 vertical
 *         blank, 0-2 version)
 *   $D601 the register's value; R31 reads or writes the RAM at the update
 *         address (R18/R19) and moves it on
 *
 * Text (R25 bit 7 clear): the screen at R12/R13, R1 characters a row, R6
 * rows of R9 + 1 lines; with attributes (R25 bit 6) one byte per character
 * at R20/R21: bits 0-3 colour (RGBI), 4 blink, 5 underline, 6 reverse,
 * 7 the second 256 characters. Characters are 16 bytes each in the set at
 * R28 bits 5-7 (8 KB steps). Bitmap (R25 bit 7): 80 bytes a line, the
 * attributes' colours per character cell. R30 starts a block fill (R31's
 * value) or copy (R24 bit 7, from R32/R33) of that many bytes.
 *
 * Drawn once a frame into an 800 x 200 picture: each character cell of 8
 * pixels 10 wide (two of its pixels doubled), so the panel's 800 pixels
 * show the 80 columns evenly; the board triples the lines.
 */
#include <string.h>
#include "cbm_internal.h"

#define VRAM_SIZE   0x10000

static uint8_t *s_vram;
static uint8_t r[38];
static uint8_t s_reg;                   /* selected register */
static uint32_t s_frames;
static bool s_dirty = true;             /* RAM or registers changed since the last picture */
static uint32_t s_signature;

/* RGBI */
static const uint16_t s_rgbi[16] = {
#define RGB(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
    RGB(0x00, 0x00, 0x00), RGB(0x55, 0x55, 0x55), RGB(0x00, 0x00, 0xAA), RGB(0x55, 0x55, 0xFF),
    RGB(0x00, 0xAA, 0x00), RGB(0x55, 0xFF, 0x55), RGB(0x00, 0xAA, 0xAA), RGB(0x55, 0xFF, 0xFF),
    RGB(0xAA, 0x00, 0x00), RGB(0xFF, 0x55, 0x55), RGB(0xAA, 0x00, 0xAA), RGB(0xFF, 0x55, 0xFF),
    RGB(0xAA, 0x55, 0x00), RGB(0xFF, 0xFF, 0x55), RGB(0xAA, 0xAA, 0xAA), RGB(0xFF, 0xFF, 0xFF),
#undef RGB
};

/* The 4416 chips (16 KB) see 14 address bits, the 4164s (64 KB) 16 */
static inline uint16_t vaddr(uint16_t a)
{
    return (r[28] & 0x10) ? a : (a & 0x3FFF);
}

static inline uint16_t reg16(int hi)
{
    return (r[hi] << 8) | r[hi + 1];
}

static inline void set_reg16(int hi, uint16_t v)
{
    r[hi] = v >> 8;
    r[hi + 1] = v & 0xFF;
}

void vdc_init(void)
{
    if (s_vram == NULL) {
        s_vram = cbm_calloc(1, VRAM_SIZE);
    }
}

void vdc_reset(void)
{
    if (s_vram) {
        memset(s_vram, 0, VRAM_SIZE);
    }
    memset(r, 0, sizeof(r));
    /* what the KERNAL sets up anyway: 80 x 25 */
    r[0] = 126; r[1] = 80; r[4] = 39; r[6] = 25; r[9] = 7; r[22] = 0x78; r[23] = 8; r[26] = 0xF0;
    r[28] = 0x20;
    s_reg = 0;
    s_dirty = true;
}

/* ---- The CPU's side ---- */

uint8_t vdc_read(uint16_t a)
{
    if (!(a & 1)) {
        /* ready; vertical blank over the VIC's last lines (the VDC keeps
         * its own time, but about the same frame rate)
         */
        int line = cbm.line;
        bool vblank = line < 16 || line >= 300;
        return 0x80 | (vblank ? 0x20 : 0) | 0x01;
    }
    switch (s_reg) {
    case 31: {
        uint16_t u = reg16(18);
        uint8_t v = s_vram[vaddr(u)];
        set_reg16(18, u + 1);
        return v;
    }
    case 16: case 17:
        return 0;                       /* light pen */
    default:
        return s_reg < sizeof(r) ? r[s_reg] : 0xFF;
    }
}

/* R30: a block of `count` bytes from the update address on */
static void block(uint8_t count)
{
    int n = count ? count : 256;
    uint16_t u = reg16(18);
    if (r[24] & 0x80) {
        uint16_t src = reg16(32);
        for (int i = 0; i < n; i++) {
            s_vram[vaddr(u++)] = s_vram[vaddr(src++)];
        }
        set_reg16(32, src);
    } else {
        for (int i = 0; i < n; i++) {
            s_vram[vaddr(u++)] = r[31];
        }
    }
    set_reg16(18, u);
}

void vdc_write(uint16_t a, uint8_t v)
{
    if (!(a & 1)) {
        s_reg = v & 0x3F;
        return;
    }
    s_dirty = true;
    switch (s_reg) {
    case 31: {
        uint16_t u = reg16(18);
        s_vram[vaddr(u)] = v;
        r[31] = v;
        set_reg16(18, u + 1);
        break;
    }
    case 30:
        r[30] = v;
        block(v);
        break;
    default:
        if (s_reg < sizeof(r)) {
            r[s_reg] = v;
        }
        break;
    }
}

/* ---- The picture ---- */

/* 8 pixels into 10: pixels 1 and 5 twice */
static inline void put_cell(uint16_t *dst, uint8_t bits, uint16_t fg, uint16_t bg)
{
    static const uint8_t src_bit[10] = { 7, 6, 6, 5, 4, 3, 2, 2, 1, 0 };
    for (int i = 0; i < 10; i++) {
        dst[i] = (bits >> src_bit[i]) & 1 ? fg : bg;
    }
}

static void draw(void)
{
    uint16_t *frame = cbm.cfg.vdc_frame;
    const int cols = r[1] > 80 ? 80 : r[1];
    const int rows = r[6];
    const int char_h = (r[9] & 0x1F) + 1;
    const int stride = r[1] + r[27];
    const bool bitmap = r[25] & 0x80, attrs = r[25] & 0x40;
    const bool reverse_all = r[24] & 0x40;
    const uint16_t screen = reg16(12), attr_base = reg16(20), cursor = reg16(14);
    const uint16_t charset = (r[28] & 0xE0) << 8;
    const int underline = r[29] & 0x1F;
    /* blink: 1/16 or 1/32 of the frame rate; the cursor (R10 bits 5-6):
     * steady, off, 1/16, 1/32
     */
    const bool blink_on = (s_frames >> ((r[24] & 0x20) ? 4 : 3)) & 1;
    const int cursor_mode = (r[10] >> 5) & 3;
    const bool cursor_on = cursor_mode == 0 || (cursor_mode >= 2 && ((s_frames >> (cursor_mode == 2 ? 3 : 4)) & 1));
    const int cursor_top = r[10] & 0x1F, cursor_bottom = r[11] & 0x1F;
    const uint16_t bg = s_rgbi[r[26] & 0x0F], fg_global = s_rgbi[r[26] >> 4];

    for (int y = 0; y < CBM_VDC_HEIGHT; y++) {
        uint16_t *dst = frame + y * CBM_VDC_WIDTH;
        int row = y / char_h, line = y % char_h;
        if (row >= rows || char_h <= 0) {
            for (int x = 0; x < CBM_VDC_WIDTH; x++) {
                dst[x] = bg;
            }
            continue;
        }
        for (int col = 0; col < 80; col++, dst += 10) {
            if (col >= cols) {
                put_cell(dst, 0, bg, bg);
                continue;
            }
            uint16_t at = row * stride + col;
            uint8_t attr = attrs ? s_vram[vaddr(attr_base + at)] : 0;
            uint16_t fg = attrs ? s_rgbi[attr & 0x0F] : fg_global;
            uint8_t bits;
            if (bitmap) {
                bits = s_vram[vaddr(screen + y * stride + col)];
            } else {
                uint8_t code = s_vram[vaddr(screen + at)];
                int ch = code | ((attr & 0x80) ? 0x100 : 0);
                bits = line < 16 ? s_vram[vaddr(charset + ch * 16 + line)] : 0;
                if ((attr & 0x20) && line == underline) {
                    bits = 0xFF;
                }
                if ((attr & 0x10) && blink_on) {
                    bits = 0;
                }
                if (attr & 0x40) {
                    bits = ~bits;
                }
                if (cursor_on && (uint16_t)(screen + at) == cursor && line >= cursor_top && line <= cursor_bottom) {
                    bits = ~bits;
                }
            }
            if (reverse_all) {
                bits = ~bits;
            }
            put_cell(dst, bits, fg, bg);
        }
    }
}

/* Drawn only while shown (`shown`: the 80-column screen is on) */
void vdc_frame_end(bool shown)
{
    s_frames++;
    if (!shown) {
        s_dirty = true;                 /* (all of it next time it's shown) */
        return;
    }
    /* blinking and the cursor change the picture too */
    bool blinking = (s_frames & 7) == 0;
    if (s_dirty || blinking) {
        s_dirty = false;
        draw();
        s_signature++;
    }
}

uint32_t vdc_signature(void)
{
    return s_signature;
}
