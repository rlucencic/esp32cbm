/*
 * The Commodore 128's MMU (8722) and its two processors' views of memory.
 *
 *   $D500 CR    configuration: bit 0 I/O at $D000 (0) or what bits 4-5
 *               say; bit 1 $4000-$7FFF BASIC low (0) or RAM; bits 2-3
 *               $8000-$BFFF BASIC high, internal function ROM, external,
 *               RAM; bits 4-5 $C000-$FFFF KERNAL (with the character ROM
 *               at $D000), internal, external, RAM; bits 6-7 the RAM bank
 *   $D501-4     PCR A-D: preset configurations (writing $FF01-$FF04 loads one)
 *   $D505 MCR   mode: bit 0 the 8502 (1) or the Z80 (0), 3 fast serial
 *               direction, 4-5 GAME/EXROM in, 6 C64 mode, 7 40/80 key in
 *   $D506 RCR   RAM: bits 0-1 common RAM 1, 4, 8, 16 KB, bit 2 at the
 *               bottom, bit 3 at the top (always bank 0), bits 6-7 the
 *               VIC's bank
 *   $D507-A     page 0 and page 1 pointers (low: page, high: bank; the
 *               high byte waits for the low one)
 *   $D50B       version ($20: 128 KB)
 *   $FF00-$FF04 CR and the LCRs, in every configuration (not in C64 mode)
 *
 * After a reset the Z80 runs first: it sees its own BIOS at $0000-$0FFF in
 * bank 0 ($D000 in the KERNAL ROM), sets things up and hands the bus to
 * the 8502, which starts from its reset vector. The Z80 reaches I/O with
 * IN and OUT ($D000-$DFFF).
 */
#include <string.h>
#include "cbm_internal.h"
#include "z80.h"

typedef struct {
    uint8_t cr, pcr[4], mcr, rcr;
    uint8_t p0l, p0h, p1l, p1h;         /* page pointers in effect */
    uint8_t p0h_latch, p1h_latch;
    bool c64_mode;
    bool z80;                           /* the Z80 has the bus */
    bool m8502_started;                 /* the 8502 has done its reset */
} mmu_t;

static mmu_t mmu;
static uint8_t s_open[256];

/* The 8502's page $FF under the MMU registers, and the Z80's view */
static const uint8_t *s_ff_rd;
static uint8_t *s_ff_wr;
static const uint8_t *s_z_rd[256];
static uint8_t *s_z_wr[256];
static uint32_t s_z_half;               /* Z80 T-states not yet a whole cycle */

/* ---- The memory map ---- */

static uint8_t *ram_page(int bank, int page)
{
    static const int common_pages[4] = { 4, 16, 32, 64 };
    int n = common_pages[mmu.rcr & 3];
    if (((mmu.rcr & 0x04) && page < n) || ((mmu.rcr & 0x08) && page >= 256 - n)) {
        bank = 0;                       /* common RAM */
    }
    return cbm.cfg.ram + ((bank & 1) << 16) + (page << 8);
}

/* What page p holds for reading in the configuration (ROMs as selected) */
static const uint8_t *rom_page(int p, bool z80)
{
    if (p >= 0x40 && p < 0x80 && !(mmu.cr & 0x02)) {
        return cbm.cfg.basic + ((p - 0x40) << 8);
    }
    if (p >= 0x80 && p < 0xC0) {
        switch ((mmu.cr >> 2) & 3) {
        case 0: return cbm.cfg.basic + 0x4000 + ((p - 0x80) << 8);
        case 1: case 2: return s_open;  /* function ROMs: none */
        default: return NULL;
        }
    }
    if (p >= 0xC0) {
        switch ((mmu.cr >> 4) & 3) {
        case 0:
            if (p >= 0xD0 && p < 0xE0 && !z80) {
                return cbm.chargen + ((p - 0xD0) << 8);     /* (the set CAPS LOCK picks) */
            }
            return cbm.cfg.kernal + ((p - 0xC0) << 8);     /* (the Z80: its BIOS at $D000) */
        case 1: case 2: return s_open;
        default: return NULL;
        }
    }
    return NULL;
}

/* The configuration the maps were made for (the KERNAL's interrupt
 * handler alone writes $FF00 twice)
 */
static struct {
    uint8_t cr, rcr, p0l, p0h, p1l, p1h;
    const uint8_t *chargen, *color;
    bool valid;
} last;

void c128_mem_update(void)
{
    if (last.valid && last.cr == mmu.cr && last.rcr == mmu.rcr && last.p0l == mmu.p0l && last.p0h == mmu.p0h &&
        last.p1l == mmu.p1l && last.p1h == mmu.p1h && last.chargen == cbm.chargen && last.color == cbm.cpu_color) {
        return;
    }
    last.cr = mmu.cr;
    last.rcr = mmu.rcr;
    last.p0l = mmu.p0l;
    last.p0h = mmu.p0h;
    last.p1l = mmu.p1l;
    last.p1h = mmu.p1h;
    last.chargen = cbm.chargen;
    last.color = cbm.cpu_color;
    last.valid = true;
    m6502_t *c = &cbm.cpu;
    int bank = mmu.cr >> 6;
    for (int p = 0; p < 256; p++) {
        uint8_t *ramp = ram_page(bank, p);
        const uint8_t *rom = rom_page(p, false);
        c->rd[p] = rom ? (uint8_t *)rom : ramp;
        c->wr[p] = ramp;                /* ROM: writes go to the RAM under it */
        if (p >= 0xD0 && p < 0xE0 && !(mmu.cr & 0x01)) {
            c->rd[p] = c->wr[p] = NULL; /* I/O */
        }
        /* the Z80 (as VICE has it): in bank 0 its BIOS ($D000 in the
         * KERNAL ROM) at $0000-$0FFF, writes there going to the RAM at
         * $D000-$DFFF (which IN and OUT to $0000-$0FFF reach too); at
         * $D000-$DFFF RAM, neither the character ROM nor I/O (IN and OUT
         * reach that); with I/O selected the colour RAM at $1000-$13FF
         */
        const uint8_t *zrom = rom_page(p, true);
        uint8_t *zwr = ramp;
        if (p < 0x10 && !(mmu.cr & 0x40)) {
            zrom = cbm.cfg.kernal + 0x1000 + (p << 8);
            zwr = ram_page(0, 0xD0 + p);
        } else if (p >= 0xD0 && p < 0xE0 && ((mmu.cr >> 4) & 3) != 1 && ((mmu.cr >> 4) & 3) != 2) {
            zrom = NULL;
        } else if (p >= 0x10 && p < 0x14 && !(mmu.cr & 0x01)) {
            /* with I/O in: the colour RAM at $1000-$13FF */
            zrom = zwr = cbm.cpu_color + ((p - 0x10) << 8);
        }
        s_z_rd[p] = zrom ? zrom : ramp;
        s_z_wr[p] = zwr;
    }
    /* page $FF: the MMU's registers at $FF00-$FF04 */
    s_ff_rd = c->rd[0xFF];
    s_ff_wr = c->wr[0xFF];
    c->rd[0xFF] = c->wr[0xFF] = NULL;
    /* pages 0 and 1 where their pointers say, and those pages swapped back */
    uint8_t *zp = cbm.cfg.ram + ((mmu.p0h & 1) << 16) + (mmu.p0l << 8);
    uint8_t *sp = cbm.cfg.ram + ((mmu.p1h & 1) << 16) + (mmu.p1l << 8);
    if (mmu.p0l != 0 && c->wr[mmu.p0l] == zp) {
        c->rd[mmu.p0l] = c->wr[mmu.p0l] = cbm.cfg.ram;
    }
    if (mmu.p1l != 1 && c->wr[mmu.p1l] == sp) {
        c->rd[mmu.p1l] = c->wr[mmu.p1l] = cbm.cfg.ram + 0x100;
    }
    cbm.zero_page = zp;
    c->rd[0] = zp;
    c->wr[0] = NULL;                    /* $00/$01: the 8502's port (bus_write) */
    c->rd[1] = c->wr[1] = sp;
    /* the VIC's 64 KB */
    uint8_t *vram = cbm.cfg.ram + ((mmu.rcr >> 6) & 1) * 0x10000;
    if (vram != vic_ram) {
        vic_catch_up();
        vic_ram = vram;
        vic_mem_changed();
    }
}

/* ---- Registers ---- */

static uint8_t mcr_read(void)
{
    uint8_t v = mmu.mcr & 0x49;                 /* CPU, fast serial direction, C64 mode */
    v |= 0x06;                                  /* unused */
    v |= cart_game ? 0x10 : 0;
    v |= cart_exrom ? 0x20 : 0;
    v |= input_40_80() ? 0 : 0x80;              /* 40/80 DISPLAY down: 80 columns */
    return v;
}

static void cpu_select(bool z80)
{
    if (z80 == mmu.z80) {
        return;
    }
    mmu.z80 = z80;
    /* the one running stops after this instruction */
    m6502_lower_until(&cbm.cpu, cbm.cpu.cycles);
    if (!z80) {
        cbm_8502_clock_resume();
    }
#ifdef CBM_MMU_LOG
    CBM_LOG("C128: the %s takes the bus", z80 ? "Z80" : "8502");
#endif
}

static void mcr_write(uint8_t v)
{
    mmu.mcr = v;
    if ((v & 0x40) && !mmu.c64_mode) {
        mmu.c64_mode = true;
        CBM_LOG("C128: C64 mode");
        c128_enter_c64_mode();
        return;
    }
    cpu_select(!(v & 0x01));
}

uint8_t c128_mmu_read(uint16_t a)
{
    if (mmu.c64_mode) {
        return 0xFF;
    }
    switch (a & 0xFF) {
    case 0x00: return mmu.cr;
    case 0x01: case 0x02: case 0x03: case 0x04: return mmu.pcr[(a & 0xFF) - 1];
    case 0x05: return mcr_read();
    case 0x06: return mmu.rcr | 0x30;
    case 0x07: return mmu.p0l;
    case 0x08: return mmu.p0h | 0xF0;
    case 0x09: return mmu.p1l;
    case 0x0A: return mmu.p1h | 0xF0;
    case 0x0B: return 0x20;                     /* 2 banks (128 KB), version 0 */
    default: return 0xFF;
    }
}

void c128_mmu_write(uint16_t a, uint8_t v)
{
#ifdef CBM_MMU_LOG
    CBM_LOG("MMU $%04X = $%02X (%s PC $%04X)", a, v, mmu.z80 ? "Z80" : "8502", mmu.z80 ? z80.pc.w : cbm.cpu.pc);
#endif
    if (mmu.c64_mode) {
        return;
    }
    switch (a & 0xFF) {
    case 0x00: mmu.cr = v; break;
    case 0x01: case 0x02: case 0x03: case 0x04: mmu.pcr[(a & 0xFF) - 1] = v; return;
    case 0x05: mcr_write(v); return;
    case 0x06: mmu.rcr = v; break;
    case 0x07: mmu.p0l = v; mmu.p0h = mmu.p0h_latch; break;
    case 0x08: mmu.p0h_latch = v & 0x0F; return;
    case 0x09: mmu.p1l = v; mmu.p1h = mmu.p1h_latch; break;
    case 0x0A: mmu.p1h_latch = v & 0x0F; return;
    default: return;
    }
    c128_mem_update();
}

/* $FF00-$FF04 (and the rest of page $FF as configured) */
uint8_t c128_page_ff_read(uint16_t a)
{
    if (a <= 0xFF04) {
        return a == 0xFF00 ? mmu.cr : mmu.pcr[a - 0xFF01];
    }
    return s_ff_rd ? s_ff_rd[a & 0xFF] : 0xFF;
}

void c128_page_ff_write(uint16_t a, uint8_t v)
{
#ifdef CBM_MMU_LOG
    CBM_LOG("MMU $%04X = $%02X (%s PC $%04X)", a, v, mmu.z80 ? "Z80" : "8502", mmu.z80 ? z80.pc.w : cbm.cpu.pc);
#endif
    if (a == 0xFF00) {
        mmu.cr = v;
    } else if (a <= 0xFF04) {
        mmu.cr = mmu.pcr[a - 0xFF01];           /* an LCR: load that preset */
    } else {
        if (s_ff_wr) {
            s_ff_wr[a & 0xFF] = v;
        }
        return;
    }
    c128_mem_update();
}

bool c128_mode(void)
{
    return cbm.cfg.model == CBM_MODEL_C128 && !mmu.c64_mode;
}

bool c128_z80_active(void)
{
    return cbm.cfg.model == CBM_MODEL_C128 && mmu.z80;
}

/* ---- The Z80 ---- */

static uint8_t z_read(uint16_t a)
{
    if ((a >> 8) == 0xFF && a <= 0xFF04) {
        return c128_page_ff_read(a);
    }
    return s_z_rd[a >> 8][a & 0xFF];
}

uint8_t c128_z80_peek(void *ctx, uint32_t a)
{
    (void)ctx;
    return z_read((uint16_t)a);
}

void c128_z80_poke(void *ctx, uint32_t a, uint8_t v)
{
    (void)ctx;
    if ((a & 0xFFFF) < 0xFF00) {
        s_z_wr[(a >> 8) & 0xFF][a & 0xFF] = v;
    }
}

#ifdef CBM_Z80IO_LOG
int c128_z80_watch = -1;
#endif

static void z_write(uint16_t a, uint8_t v)
{
#ifdef CBM_Z80IO_LOG
    if ((a & 0xFFF8) == (uint16_t)c128_z80_watch) {
        CBM_LOG("Z80 write $%04X = $%02X (PC $%04X, CR $%02X, RCR $%02X)", a, v, z80.pc.w, mmu.cr, mmu.rcr);
    }
#endif
    if ((a >> 8) == 0xFF && a <= 0xFF04) {
        c128_page_ff_write(a, v);
        return;
    }
    s_z_wr[a >> 8][a & 0xFF] = v;
}

/* I/O: $D000-$DFFF as the 8502 sees it with I/O in */
#ifdef CBM_Z80IO_LOG
bool c128_z80io_log;
#endif

/* Bank 0's RAM under the BIOS (at $D000-$DFFF), for IN and OUT to $0xxx */
static uint8_t *bios_ram(uint16_t port)
{
    return mmu.cr & 0x40 ? NULL : ram_page(0, 0xD0 + ((port >> 8) & 0x0F)) + (port & 0xFF);
}

static uint8_t z_in(uint16_t port, int at)
{
    if (port < 0x1000) {
        uint8_t *m = bios_ram(port);
        return m ? *m : z_read(port);
    }
    if ((port & 0xF000) == 0xD000) {
        uint8_t v = cbm_io_read(port);
#ifdef CBM_Z80IO_LOG
        if (c128_z80io_log && ((port & 0xFF00) == 0xDC00 || port == 0xD02F)) {
            CBM_LOG("Z80 IN  $%04X = $%02X (PC $%04X)", port, v, z80.pc.w);
        }
#endif
        return v;
    }
    return 0xFF;
}

static void z_out(uint16_t port, uint8_t v, int at)
{
#ifdef CBM_Z80IO_LOG
    if (c128_z80io_log && ((port & 0xFF00) == 0xDC00 || port == 0xD02F)) {
        CBM_LOG("Z80 OUT $%04X = $%02X (PC $%04X)", port, v, z80.pc.w);
    }
#endif
    if (port < 0x1000) {
        uint8_t *m = bios_ram(port);
        if (m) {
            *m = v;
        } else {
            z_write(port, v);
        }
    } else if ((port & 0xF000) == 0xD000) {
        cbm_io_write(port, v);
    }
}

static uint8_t z_int_ack(void)
{
    return 0xFF;
}

/* The Z80 at twice the 8502's clock (it gets about 2 of its 4 MHz) up to
 * `until`, or until it hands over the bus
 */
void c128_z80_run(uint64_t until)
{
    while (mmu.z80 && cbm.clock < until) {
        z80.int_line = cbm.cpu.irq != 0;
        s_z_half += z80_step();
        cbm.clock += s_z_half >> 1;
        s_z_half &= 1;
    }
    cbm_8502_clock_resume();
}

/* The 8502 gets the bus: its reset sequence the first time */
void c128_8502_start(void)
{
    if (!mmu.m8502_started) {
        mmu.m8502_started = true;
        m6502_reset(&cbm.cpu);
    }
}

void c128_init(void)
{
    memset(s_open, 0xFF, sizeof(s_open));
    static const z80_bus_t bus = {
        .in = z_in, .out = z_out, .int_ack = z_int_ack, .write_special = z_write, .read_special = z_read,
    };
    z80_set_bus(&bus);
    for (int i = 0; i < 4; i++) {
        z80_rd[i] = NULL;
        z80_wr[i] = NULL;
    }
}

void c128_reset(void)
{
    memset(&mmu, 0, sizeof(mmu));
    last.valid = false;
    mmu.p1l = 1;
    mmu.z80 = true;                     /* the Z80 boots */
    s_z_half = 0;
    z80_reset();
    c128_mem_update();
}

bool c128_fast_serial_out(void)
{
    return mmu.mcr & 0x08;
}

/* From a 1571: into CIA 1's shift register, if the C128 listens */
void c128_fast_in(uint8_t byte)
{
    if (cbm.cfg.model == CBM_MODEL_C128 && !(mmu.mcr & 0x08)) {
        cia_sdr_in(&cbm.cia1, byte);
        cbm_events_changed();
    }
}

uint16_t c128_z80_pc(void)
{
    return z80.pc.w;
}

bool c128_in_c64_mode(void)
{
    return mmu.c64_mode;
}
