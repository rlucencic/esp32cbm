/*
 * VIC-II (6569, PAL): 312 lines of 63 cycles, 8 pixels a cycle.
 *
 * Per raster line: the raster interrupt, the bad line condition (the
 * c-accesses read the 40 screen codes and colours; the CPU's reads stall
 * in cycles 12-54), sprite DMA (two cycles a sprite, three more where BA
 * goes low first), VC/VCBASE/RC, and the sprites' MC/MCBASE and Y
 * expansion as in Christian Bauer's "VIC-II article".
 *
 * The picture is drawn as the beam goes: vic_catch_up() draws the current
 * line up to where the beam is (a register write calls it first, so colour
 * and mode changes within a line land where they happen). Pixels are
 * colour indices per 8-pixel character, then RGB565; a row is only written
 * where it changed. The window is X -16 .. 383 (sprite coordinates) and
 * raster lines 8 .. 307.
 */
#include <string.h>
#include "cbm_internal.h"

#define WIN_W           CBM_SCREEN_WIDTH        /* 400 */
#define WIN_X0          (-16)                   /* X at window x 0 */
#define WIN_Y0          8                       /* raster line at window y 0 */
#define X_WRAP          0x1F8                   /* 504 X positions a line */

uint8_t *vic_ram;
const uint8_t *vic_chargen;
const uint8_t *vic_color;
bool vic_rom_chars = true;
bool vic_chars_all_banks;
static uint32_t s_mem_id;               /* changes with those (the line keys) */
uint8_t vic_d02f = 0xFF, vic_d030 = 0xFC;

static uint8_t r[0x40];
static uint8_t s_bank;                  /* 0-3: VIC address bits 14-15 */
static int s_line;
static uint8_t s_irq;                   /* $D019 bits 0-3 */
static bool s_raster_irq_done;
static bool s_den_seen, s_badline, s_display;
static uint16_t s_vc, s_vcbase;
static uint8_t s_rc;
static uint8_t s_vm[40], s_col[40];
static bool s_vborder, s_mborder;
static uint32_t s_sig;
static bool s_dirty;

/* Sprites */
static uint8_t s_mc[8], s_mcbase[8];
static bool s_dma[8], s_disp[8], s_yff[8];
static uint32_t s_sdata[8];             /* 24 bits fetched for this line */
static uint8_t s_sline;                 /* sprites drawn on this line */

/* The line being drawn: colour indices and foreground flags */
static uint8_t s_pix[WIN_W] __attribute__((aligned(4)));
static uint32_t *s_hash;                /* of each line as last written (PSRAM) */
static uint8_t s_fg[WIN_W];             /* graphics foreground (for priority, collisions) */
static uint16_t *s_out;                 /* the frame's row, NULL outside the window */
static int s_x;                         /* drawn up to window x */
static uint64_t s_stall;                /* cycles of this line the CPU can't read (bit k = cycle k+1) */

/* Pepto's PAL palette */
static const uint16_t s_rgb[16] = {
#define RGB(r, g, b)    (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
    RGB(0x00, 0x00, 0x00), RGB(0xFF, 0xFF, 0xFF), RGB(0x68, 0x37, 0x2B), RGB(0x70, 0xA4, 0xB2),
    RGB(0x6F, 0x3D, 0x86), RGB(0x58, 0x8D, 0x43), RGB(0x35, 0x28, 0x79), RGB(0xB8, 0xC7, 0x6F),
    RGB(0x6F, 0x4F, 0x25), RGB(0x43, 0x39, 0x00), RGB(0x9A, 0x67, 0x59), RGB(0x44, 0x44, 0x44),
    RGB(0x6C, 0x6C, 0x6C), RGB(0x9A, 0xD2, 0x84), RGB(0x6C, 0x5E, 0xB5), RGB(0x95, 0x95, 0x95),
#undef RGB
};

/* ---- Memory as the VIC sees it ---- */

static inline uint8_t vmem(uint16_t a)
{
    a &= 0x3FFF;
    if ((a & 0x3000) == 0x1000 && vic_rom_chars && (!(s_bank & 1) || vic_chars_all_banks)) {
        return vic_chargen[a & 0x0FFF];         /* character ROM in banks 0 and 2 */
    }
    return vic_ram[(s_bank << 14) | a];
}

void vic_mem_changed(void)
{
    s_mem_id++;
}

void vic_set_bank(uint8_t bank)
{
    vic_catch_up();
    s_bank = bank & 3;
}

/* ---- Interrupts ---- */

static void update_irq(void)
{
    if (s_irq & r[0x1A] & 0x0F) {
        cbm.cpu.irq |= IRQ_VIC;
    } else {
        cbm.cpu.irq &= ~IRQ_VIC;
    }
}

static inline uint16_t irq_line(void)
{
    return r[0x12] | ((r[0x11] & 0x80) << 1);
}

static void raster_compare(void)
{
    if (s_line == irq_line()) {
        if (!s_raster_irq_done) {
            s_raster_irq_done = true;
            s_irq |= 0x01;
            update_irq();
        }
    } else {
        s_raster_irq_done = false;
    }
}

/* ---- Drawing ---- */

static inline int x_of_cycle(int k)
{
    /* cycle k+1 starts at X $194 + 8k (mod 504) */
    int x = (0x194 + 8 * k) % X_WRAP;
    if (x >= X_WRAP + WIN_X0) {
        x -= X_WRAP;
    }
    return x - WIN_X0;
}

/* Bit masks: s_mask[b] has byte i = $FF where bit 7-i of b is set (pixel
 * order, little-endian)
 */
static uint64_t s_mask[256];
#define REP(c)          ((uint64_t)(c) * 0x0101010101010101ULL)

static void init_masks(void)
{
    for (int b = 0; b < 256; b++) {
        uint64_t m = 0;
        for (int i = 0; i < 8; i++) {
            if (b & (0x80 >> i)) {
                m |= 0xFFULL << (i * 8);
            }
        }
        s_mask[b] = m;
    }
}

/* The graphics byte of a column (g-access) */
static inline uint8_t g_byte(int col, uint8_t mode, uint16_t cb)
{
    if (!s_display) {
        return vmem((r[0x11] & 0x40) ? 0x39FF : 0x3FFF);    /* idle state */
    }
    if (mode & 2) {
        return vmem((cb & 0x2000) | (((s_vcbase + col) & 0x3FF) << 3) | s_rc);
    }
    uint8_t c = s_vm[col];
    return vmem(cb | ((mode & 4 ? c & 0x3F : c) << 3) | s_rc);
}

/* Multicolour: the two bits of each pixel pair as masks */
static inline void mc_masks(uint8_t data, uint64_t *hi, uint64_t *lo)
{
    uint64_t h = s_mask[data & 0xAA], l = s_mask[data & 0x55];
    *hi = h | (h << 8);
    *lo = l | (l >> 8);
}

/* One character's 8 pixels (colour indices) and foreground flags */
static inline void char_pixels(int col, uint8_t mode, const uint8_t bg[4], uint16_t cb, uint64_t *px, uint64_t *fg)
{
    uint8_t data = g_byte(col, mode, cb);
    uint8_t c = s_vm[col], color = s_col[col];
    if (!s_display || mode >= 5) {
        *px = 0;                        /* idle state and the invalid modes: black */
        if (mode & 1) {
            uint64_t hi, lo;
            mc_masks(data, &hi, &lo);
            *fg = hi & REP(1);
        } else {
            *fg = s_mask[data] & REP(1);
        }
        return;
    }
    bool multi = (mode & 1) && ((mode & 2) || (color & 8));
    if (multi) {
        uint64_t hi, lo;
        mc_masks(data, &hi, &lo);
        uint8_t c1, c2, c3;
        if (mode & 2) {
            c1 = c >> 4;
            c2 = c & 15;
            c3 = color & 15;
        } else {
            c1 = bg[1];
            c2 = bg[2];
            c3 = color & 7;
        }
        *px = (REP(bg[0]) & ~hi & ~lo) | (REP(c1) & ~hi & lo) | (REP(c2) & hi & ~lo) | (REP(c3) & hi & lo);
        *fg = hi & REP(1);
        return;
    }
    uint8_t on, off;
    switch (mode) {
    case 2: case 3: on = c >> 4; off = c & 15; break;       /* bitmap */
    case 4: on = color & 15; off = bg[c >> 6]; break;       /* extended background colour */
    case 1: on = color & 7; off = bg[0]; break;             /* multicolour text, hires character */
    default: on = color & 15; off = bg[0]; break;           /* text */
    }
    uint64_t m = s_mask[data];
    *px = (REP(on) & m) | (REP(off) & ~m);
    *fg = m & REP(1);
}

/* The 40 columns' pixels, drawn once a line for the columns asked */
static uint64_t s_gpix[40], s_gfg[40];

/* Background and graphics for window pixels [x0, x1) */
static void draw_graphics(int x0, int x1)
{
    uint8_t mode = ((r[0x11] & 0x60) | (r[0x16] & 0x10)) >> 4;      /* ECM BMM MCM */
    const uint8_t bg[4] = { r[0x21] & 15, r[0x22] & 15, r[0x23] & 15, r[0x24] & 15 };
    uint16_t cb = (r[0x18] & 0x0E) << 10;
    int gx0 = 24 - WIN_X0 + (r[0x16] & 7);  /* window x of the first graphics pixel */
    int gend = gx0 + 320;
    int x = x0;
    if (x < gx0) {
        int end = gx0 < x1 ? gx0 : x1;
        memset(s_pix + x, bg[0], end - x);
        memset(s_fg + x, 0, end - x);
        x = end;
    }
    int end = x1 < gend ? x1 : gend;
    if (x < end) {
        int c0 = (x - gx0) >> 3, c1 = (end - gx0 + 7) >> 3;
        for (int col = c0; col < c1; col++) {
            char_pixels(col, mode, bg, cb, &s_gpix[col], &s_gfg[col]);
        }
        memcpy(s_pix + x, (const uint8_t *)s_gpix + (x - gx0), end - x);
        if (s_sline) {
            memcpy(s_fg + x, (const uint8_t *)s_gfg + (x - gx0), end - x);
        }
        x = end;
    }
    if (x < x1) {
        memset(s_pix + x, bg[0], x1 - x);
        memset(s_fg + x, 0, x1 - x);
    }
}

/* What a line without sprites, drawn in one go, depends on: if it's the
 * same as last frame's, the line is too (s_key per window line)
 */
static uint32_t *s_key;                 /* (PSRAM: once a line) */
static uint8_t *s_key_border;           /* the flip-flops after it */

static uint32_t line_key(void)
{
    uint32_t h = 2166136261u;
#define MIX(v)  (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIX(r[0x11] & 0x78);
    MIX(r[0x16] & 0x1F);
    MIX(r[0x18]);
    MIX(r[0x20] | (r[0x21] << 4) | ((uint32_t)r[0x22] << 8) | ((uint32_t)r[0x23] << 12) | ((uint32_t)r[0x24] << 16));
    MIX(s_vborder | (s_mborder << 1) | (s_display << 2) | (s_bank << 3) | (s_mem_id << 5));
    bool rsel = r[0x11] & 0x08;
    if (s_vborder && s_mborder && s_line != (rsel ? 51 : 55) && s_line != (rsel ? 251 : 247)) {
        return h;                       /* all border */
    }
    uint8_t mode = ((r[0x11] & 0x60) | (r[0x16] & 0x10)) >> 4;
    uint16_t cb = (r[0x18] & 0x0E) << 10;
    for (int col = 0; col < 40; col++) {
        MIX(g_byte(col, mode, cb) | (s_vm[col] << 8) | ((uint32_t)s_col[col] << 16));
    }
#undef MIX
    return h;
}

/* Sprites over [x0, x1); collisions */
static void draw_sprites(int x0, int x1)
{
    if (!s_sline) {
        return;
    }
    uint8_t sb = 0;
    for (int i = 7; i >= 0; i--) {
        if (!(s_sline & (1u << i))) {
            continue;
        }
        int sx = r[i * 2] | ((r[0x10] >> i) & 1) << 8;
        if (sx >= X_WRAP + WIN_X0) {
            sx -= X_WRAP;
        }
        int wx = sx - WIN_X0;
        bool xexp = (r[0x1D] >> i) & 1, mc = (r[0x1C] >> i) & 1, behind = (r[0x1B] >> i) & 1;
        int w = xexp ? 48 : 24;
        int a = wx > x0 ? wx : x0, b = wx + w < x1 ? wx + w : x1;
        uint8_t colors[4] = { 0, r[0x25] & 15, r[0x27 + i] & 15, r[0x26] & 15 };
        uint32_t d = s_sdata[i];
        for (int x = a; x < b; x++) {
            int bitpos = (x - wx) >> xexp;
            uint8_t v;
            if (mc) {
                v = (d >> (22 - (bitpos & ~1))) & 3;
            } else {
                v = ((d >> (23 - bitpos)) & 1) ? 2 : 0;
            }
            if (!v) {
                continue;
            }
            if (s_fg[x]) {
                sb |= 1u << i;          /* sprite-background collision */
            }
            if (!(behind && s_fg[x])) {
                s_pix[x] = colors[v];
            }
        }
    }
    if (sb) {
        if (!r[0x1F]) {
            s_irq |= 0x02;
            update_irq();
        }
        r[0x1F] |= sb;
    }
}

/* Sprite-sprite collisions need the sprites' own bits per pixel */
static void sprite_collisions(int x0, int x1)
{
    if (!s_sline || !(s_sline & (s_sline - 1))) {
        return;
    }
    uint8_t ss = 0;
    for (int x = x0; x < x1; x++) {
        uint8_t m = 0;
        for (int i = 0; i < 8; i++) {
            if (!(s_sline & (1u << i))) {
                continue;
            }
            int sx = r[i * 2] | ((r[0x10] >> i) & 1) << 8;
            if (sx >= X_WRAP + WIN_X0) {
                sx -= X_WRAP;
            }
            int dx = x - (sx - WIN_X0);
            bool xexp = (r[0x1D] >> i) & 1;
            if (dx < 0 || dx >= (xexp ? 48 : 24)) {
                continue;
            }
            int bitpos = dx >> xexp;
            bool on = (r[0x1C] >> i) & 1 ? ((s_sdata[i] >> (22 - (bitpos & ~1))) & 3) != 0
                                         : ((s_sdata[i] >> (23 - bitpos)) & 1);
            if (on) {
                m |= 1u << i;
            }
        }
        if (m & (m - 1)) {
            ss |= m;
        }
    }
    if (ss) {
        if (!r[0x1E]) {
            s_irq |= 0x04;
            update_irq();
        }
        r[0x1E] |= ss;
    }
}

/* The border over [x0, x1): the main border flip-flop at the left and
 * right comparison positions, the vertical one at the left
 */
static void draw_border(int x0, int x1)
{
    bool csel = r[0x16] & 0x08, rsel = r[0x11] & 0x08;
    int left = (csel ? 24 : 31) - WIN_X0, right = (csel ? 344 : 335) - WIN_X0;
    int top = rsel ? 51 : 55, bottom = rsel ? 251 : 247;
    uint8_t bc = r[0x20] & 15;
    for (int x = x0; x < x1; ) {
        if (x == right) {
            s_mborder = true;
        }
        if (x == left) {
            if (s_line == bottom) {
                s_vborder = true;
            } else if (s_line == top && (r[0x11] & 0x10)) {
                s_vborder = false;
            }
            if (!s_vborder) {
                s_mborder = false;
            }
        }
        /* up to the next comparison position */
        int end = x1;
        if (x < left && left < end) {
            end = left;
        }
        if (x < right && right < end) {
            end = right;
        }
        if (end <= x) {
            end = x + 1;
        }
        if (s_mborder) {
            memset(s_pix + x, bc, end - x);
        }
        x = end;
    }
}

static void draw(int x1)
{
    if (x1 > WIN_W) {
        x1 = WIN_W;
    }
    if (x1 <= s_x) {
        return;
    }
    int x0 = s_x;
    s_x = x1;
    if (s_out == NULL) {
        return;
    }
    bool rsel = r[0x11] & 0x08;
    if (s_vborder && s_mborder && s_line != (rsel ? 51 : 55) && s_line != (rsel ? 251 : 247)) {
        memset(s_pix + x0, r[0x20] & 15, x1 - x0);  /* all border */
        return;
    }
    draw_graphics(x0, x1);
    sprite_collisions(x0, x1);
    draw_sprites(x0, x1);
    draw_border(x0, x1);
}

/* The finished line into the frame, if it changed */
static void commit_line(void)
{
    if (s_out == NULL) {
        return;
    }
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)s_pix;
    for (int i = 0; i < WIN_W / 4; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    int y = s_line - WIN_Y0;
    if (h == s_hash[y]) {
        return;
    }
    s_hash[y] = h;
    uint32_t *o = (uint32_t *)s_out;
    for (int x = 0; x < WIN_W; x += 2) {
        o[x >> 1] = s_rgb[s_pix[x]] | ((uint32_t)s_rgb[s_pix[x + 1]] << 16);
    }
    s_dirty = true;
}

void vic_catch_up(void)
{
    int k = cbm_line_cycle();
    if (k < 0) {
        return;
    }
    if (k > 62) {
        k = 62;
    }
    /* the window starts at cycle 11 and ends in cycle 61 */
    int x = k < 10 ? 0 : x_of_cycle(k);
    if (k >= 10 && x < 0) {
        x = 0;
    }
    draw(x);
}

/* ---- Lines ---- */

/* Cycles of this line the CPU can't read in: bad line, sprite DMA */
static void make_stalls(void)
{
    uint64_t m = 0;
    if (s_badline) {
        m |= ((1ULL << 43) - 1) << 11;          /* cycles 12-54 */
    }
    for (int i = 0; i < 8; i++) {
        bool dma_now = s_dma[i];                /* sprites 3-7: fetched in cycles 1-10 */
        bool dma_end = s_dma[i] || ((r[0x15] >> i) & 1 && r[1 + i * 2] == (s_line & 0xFF));
        int start;
        if (i < 3) {
            if (!dma_end) {
                continue;
            }
            start = 57 + 2 * i;                 /* cycles 58, 60, 62 */
        } else {
            if (!dma_now) {
                continue;
            }
            start = 2 * (i - 3);                /* cycles 1, 3, 5, 7, 9 */
        }
        int from = start - 3 < 0 ? 0 : start - 3;
        for (int k = from; k <= start + 1 && k < 63; k++) {
            m |= 1ULL << k;
        }
    }
    s_stall = m;
    /* (at the C128's 2 MHz the VIC gets no cycles from the CPU) */
    m6502_set_stall_from(&cbm.cpu, m && !cbm.fast ? cbm.line_start + __builtin_ctzll(m) : UINT64_MAX);
}

void vic_cpu_stall(void *ctx, m6502_t *cpu)
{
    int k = (int)(cpu->cycles - cbm.line_start);
    if (k < 0 || k > 62) {
        m6502_set_stall_from(cpu, UINT64_MAX);
        return;
    }
    while (k < 63 && (s_stall & (1ULL << k))) {
        k++;
    }
    cpu->cycles = cbm.line_start + k;
    uint64_t rest = k < 63 ? s_stall >> k : 0;
    m6502_set_stall_from(cpu, rest ? cpu->cycles + __builtin_ctzll(rest) : UINT64_MAX);
}

void vic_line_start(int line)
{
    s_line = line;
    if (line == 0) {
        s_vcbase = 0;
    }
    raster_compare();
    if (line == 0x30) {
        s_den_seen = r[0x11] & 0x10;
    }
    /* sprites: cycles 15 and 16 */
    for (int i = 0; i < 8; i++) {
        if (s_yff[i]) {
            s_mcbase[i] += 3;
            if (s_mcbase[i] >= 63) {
                s_mcbase[i] = 63;
                s_dma[i] = s_disp[i] = false;
            }
        }
    }
    s_badline = line >= 0x30 && line <= 0xF7 && (line & 7) == (r[0x11] & 7) && s_den_seen;
    s_vc = s_vcbase;
    if (s_badline) {
        s_display = true;
        s_rc = 0;
        uint16_t vm = (r[0x18] & 0xF0) << 6;
        for (int i = 0; i < 40; i++) {
            s_vm[i] = vmem(vm | ((s_vc + i) & 0x3FF));
            s_col[i] = vic_color[(s_vc + i) & 0x3FF] & 0x0F;    /* (the C128's Z80 stores whole bytes) */
        }
    }
    make_stalls();
    int y = line - WIN_Y0;
    s_out = y >= 0 && y < CBM_SCREEN_HEIGHT ? cbm.cfg.frame + y * CBM_SCREEN_WIDTH : NULL;
    s_x = 0;
}

void vic_line_end(void)
{
    if (s_out && s_x == 0 && !s_sline) {
        int y = s_line - WIN_Y0;
        uint32_t key = line_key();
        if (key == s_key[y]) {
            s_mborder = s_key_border[y] & 1;    /* as drawing it would have left them */
            s_vborder = s_key_border[y] >> 1;
            s_x = WIN_W;
        } else {
            draw(WIN_W);
            commit_line();
            s_key[y] = key;
            s_key_border[y] = s_mborder | (s_vborder << 1);
        }
    } else {
        draw(WIN_W);
        commit_line();
        if (s_out) {
            s_key[s_line - WIN_Y0] = 0;     /* drawn in parts or with sprites: no key */
        }
    }
    /* cycle 58: RC and VCBASE */
    if (s_display) {
        if (s_rc == 7) {
            s_vcbase = s_vc + 40;
            if (!s_badline) {
                s_display = false;
            }
        }
        s_rc = (s_rc + 1) & 7;
    }
    if (s_badline) {
        s_display = true;
    }
    /* cycle 63: the vertical border flip-flop */
    bool rsel = r[0x11] & 0x08;
    if (s_line == (rsel ? 251 : 247)) {
        s_vborder = true;
    } else if (s_line == (rsel ? 51 : 55) && (r[0x11] & 0x10)) {
        s_vborder = false;
    }
    /* sprites: cycles 55-58, then the s-accesses for the next line */
    s_sline = 0;
    uint16_t vm = (r[0x18] & 0xF0) << 6;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = 1u << i;
        bool yexp = r[0x17] & bit;
        if (!yexp) {
            s_yff[i] = true;
        } else {
            s_yff[i] = !s_yff[i];
        }
        if ((r[0x15] & bit) && r[1 + i * 2] == (s_line & 0xFF) && !s_dma[i]) {
            s_dma[i] = true;
            s_mcbase[i] = 0;
            if (yexp) {
                s_yff[i] = false;
            }
        }
        s_mc[i] = s_mcbase[i];
        if (s_dma[i] && r[1 + i * 2] == (s_line & 0xFF)) {
            s_disp[i] = true;
        }
        if (s_dma[i]) {
            uint16_t ptr = vmem(vm | 0x3F8 | i) << 6;
            uint8_t mc = s_mc[i];
            s_sdata[i] = (vmem(ptr | mc) << 16) | (vmem(ptr | ((mc + 1) & 63)) << 8) | vmem(ptr | ((mc + 2) & 63));
            if (s_disp[i]) {
                s_sline |= bit;
            }
        }
    }
}

void vic_frame_end(void)
{
    if (s_dirty) {
        s_dirty = false;
        s_sig++;
    }
}

uint32_t vic_signature(void)
{
    return s_sig;
}

uint16_t vic_raster(void)
{
    return s_line;
}

/* ---- Registers ---- */

uint8_t vic_read(uint8_t reg)
{
    reg &= 0x3F;
    switch (reg) {
    case 0x11: return (r[0x11] & 0x7F) | ((s_line & 0x100) >> 1);
    case 0x12: return s_line & 0xFF;
    case 0x16: return r[0x16] | 0xC0;
    case 0x18: return r[0x18] | 0x01;
    case 0x19: return s_irq | 0x70 | ((s_irq & r[0x1A] & 0x0F) ? 0x80 : 0);
    case 0x1A: return r[0x1A] | 0xF0;
    case 0x1E: case 0x1F: {
        vic_catch_up();
        uint8_t v = r[reg];
        r[reg] = 0;
        return v;
    }
    case 0x2F: return cbm.cfg.model == CBM_MODEL_C128 ? vic_d02f : 0xFF;
    case 0x30: return cbm.cfg.model == CBM_MODEL_C128 ? vic_d030 : 0xFF;
    default:
        if (reg >= 0x20 && reg <= 0x2E) {
            return r[reg] | 0xF0;
        }
        return reg < 0x20 ? r[reg] : 0xFF;
    }
}

void vic_write(uint8_t reg, uint8_t v)
{
    reg &= 0x3F;
    vic_catch_up();
    switch (reg) {
    case 0x11:
        r[0x11] = v;
        if (s_line == 0x30 && (v & 0x10)) {
            s_den_seen = true;
        }
        raster_compare();
        break;
    case 0x12:
        r[0x12] = v;
        raster_compare();
        break;
    case 0x19:
        s_irq &= ~(v & 0x0F);           /* writing 1 acknowledges */
        update_irq();
        break;
    case 0x1A:
        r[0x1A] = v & 0x0F;
        update_irq();
        break;
    case 0x1E: case 0x1F:
        break;                          /* read-only */
    case 0x2F: vic_d02f = v; break;
    case 0x30:
        vic_d030 = v;
        cbm_fast_request(v & 1);
        break;
    default:
        if (reg < 0x2F) {
            r[reg] = v;
        }
        break;
    }
}

void vic_init(void)
{
    init_masks();
    if (s_hash == NULL) {
        s_hash = cbm_malloc(CBM_SCREEN_HEIGHT * sizeof(uint32_t));
        s_key = cbm_malloc(CBM_SCREEN_HEIGHT * sizeof(uint32_t));
        s_key_border = cbm_malloc(CBM_SCREEN_HEIGHT);
    }
    vic_reset();
}

void vic_reset(void)
{
    memset(r, 0, sizeof(r));
    s_irq = 0;
    s_line = 0;
    s_bank = 0;
    s_den_seen = s_badline = s_display = false;
    s_vc = s_vcbase = 0;
    s_rc = 0;
    s_vborder = s_mborder = true;
    memset(s_dma, 0, sizeof(s_dma));
    memset(s_disp, 0, sizeof(s_disp));
    memset(s_mcbase, 0, sizeof(s_mcbase));
    s_sline = 0;
    vic_d02f = 0xFF;
    vic_d030 = 0xFC;
    s_dirty = true;
    memset(s_hash, 0xFF, CBM_SCREEN_HEIGHT * sizeof(uint32_t));
    memset(s_key, 0, CBM_SCREEN_HEIGHT * sizeof(uint32_t));
    update_irq();
}
