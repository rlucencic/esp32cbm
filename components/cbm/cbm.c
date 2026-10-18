/*
 * The machine: memory map (the 6510's port and the PLA), I/O, the CIAs'
 * wiring (keyboard, joysticks, VIC bank, serial bus), interrupts and the
 * frame loop: 312 raster lines of 63 cycles.
 *
 *   $D000 VIC-II   $D400 SID   $D800 colour RAM   $DC00 CIA 1   $DD00 CIA 2
 *   $DE00/$DF00 cartridge I/O
 *
 * The C128 (c128.c) adds $D500 MMU and $D600 VDC; in C128 mode its MMU
 * makes the memory map, in C64 mode the PLA as here.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cbm_internal.h"
#include "emudbg.h"
#ifdef CBM_PROFILE
uint32_t cbm_prof_drive, cbm_prof_vic, cbm_prof_frame, cbm_prof_drive_steps;
uint32_t cbm_prof_io, cbm_prof_drive_io, cbm_prof_drives_calls;
#endif

cbm_t cbm;

static uint64_t s_next_event;           /* the earliest CIA timer underflow */
static uint8_t s_open_bus[256];         /* unmapped (Ultimax): reads $FF */

/* ---- Memory ---- */

enum { M_RAM, M_ROML, M_ROMH, M_BASIC, M_KERNAL, M_CHAR, M_IO, M_OPEN };

/* The PLA by EXROM GAME CHAREN HIRAM LORAM: what's at $8000, $A000,
 * $D000, $E000 (Ultimax also leaves $1000-$7FFF and $C000 unmapped)
 */
static const uint8_t pla[32][4] = {
    /* EXROM 0, GAME 0: 16 KB cartridge */
    [0] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [1] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [2] = { M_RAM, M_ROMH, M_CHAR, M_KERNAL },
    [3] = { M_ROML, M_ROMH, M_CHAR, M_KERNAL },
    [4] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [5] = { M_RAM, M_RAM, M_IO, M_RAM },
    [6] = { M_RAM, M_ROMH, M_IO, M_KERNAL },
    [7] = { M_ROML, M_ROMH, M_IO, M_KERNAL },
    /* EXROM 0, GAME 1: 8 KB cartridge */
    [8] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [9] = { M_RAM, M_RAM, M_CHAR, M_RAM },
    [10] = { M_RAM, M_RAM, M_CHAR, M_KERNAL },
    [11] = { M_ROML, M_BASIC, M_CHAR, M_KERNAL },
    [12] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [13] = { M_RAM, M_RAM, M_IO, M_RAM },
    [14] = { M_RAM, M_RAM, M_IO, M_KERNAL },
    [15] = { M_ROML, M_BASIC, M_IO, M_KERNAL },
    /* EXROM 1, GAME 0: Ultimax */
    [16 ... 23] = { M_ROML, M_OPEN, M_IO, M_ROMH },
    /* EXROM 1, GAME 1: no cartridge */
    [24] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [25] = { M_RAM, M_RAM, M_CHAR, M_RAM },
    [26] = { M_RAM, M_RAM, M_CHAR, M_KERNAL },
    [27] = { M_RAM, M_BASIC, M_CHAR, M_KERNAL },
    [28] = { M_RAM, M_RAM, M_RAM, M_RAM },
    [29] = { M_RAM, M_RAM, M_IO, M_RAM },
    [30] = { M_RAM, M_RAM, M_IO, M_KERNAL },
    [31] = { M_RAM, M_BASIC, M_IO, M_KERNAL },
};

static uint8_t port_config(void)
{
    /* inputs read high (pull-ups) */
    return (cbm.port_data | ~cbm.port_ddr) & 7;
}

static void map(int first_page, int pages, int what, const uint8_t *rom)
{
    m6502_t *c = &cbm.cpu;
    uint8_t *ram = cbm.cfg.ram;
    for (int i = 0; i < pages; i++) {
        int p = first_page + i;
        switch (what) {
        case M_RAM:
            c->rd[p] = c->wr[p] = ram + p * 256;
            break;
        case M_IO:
            c->rd[p] = c->wr[p] = NULL;
            break;
        case M_OPEN:
            c->rd[p] = s_open_bus;
            c->wr[p] = NULL;
            break;
        default:
            c->rd[p] = rom ? (uint8_t *)rom + i * 256 : s_open_bus;
            c->wr[p] = ram + p * 256;   /* ROM: writes go to the RAM under it */
            break;
        }
    }
}

void mem_update(void)
{
    if (c128_mode()) {
        c128_mem_update();
        return;
    }
    int mode = (cart_exrom << 4) | (cart_game << 3) | port_config();
    const uint8_t *m = pla[mode];
    bool ultimax = cart_exrom && !cart_game;
    map(0x00, 0x80, M_RAM, NULL);
    if (ultimax) {
        map(0x10, 0x70, M_OPEN, NULL);
        map(0xC0, 0x10, M_OPEN, NULL);
    } else {
        map(0xC0, 0x10, M_RAM, NULL);
    }
    map(0x80, 0x20, m[0], cart_roml());
    map(0xA0, 0x20, m[1], m[1] == M_BASIC ? cbm.basic : cart_romh());
    map(0xD0, 0x10, m[2], cbm.chargen);
    map(0xE0, 0x20, m[3], m[3] == M_KERNAL ? cbm.kernal : cart_romh());
    if (ultimax) {
        for (int p = 0x80; p < 0xA0; p++) {
            cbm.cpu.wr[p] = NULL;       /* the cartridge's ROM, not RAM */
        }
        for (int p = 0xE0; p < 0x100; p++) {
            cbm.cpu.wr[p] = NULL;
        }
    }
    /* $00/$01 are the CPU's port: writes go through bus_write */
    cbm.cpu.wr[0] = NULL;
}

uint8_t cbm_peek(uint16_t address)
{
    if (address < 2) {
        return cbm.zero_page[address];
    }
    const uint8_t *p = cbm.cpu.rd[address >> 8];
    if (p) {
        return p[address & 0xFF];
    }
    if (address >= 0xD800 && address < 0xDC00) {
        return cbm.cpu_color[address & 0x3FF] | 0xF0;
    }
    if (address >= 0xFF00 && c128_mode()) {
        return c128_page_ff_read(address);
    }
    return 0xFF;
}

/* The port's read values live in RAM $00/$01 (the CPU reads page 0 from
 * RAM directly)
 */
static void port_changed(void)
{
    /* bits 0-2 and 4 (cassette sense: no key) read high as inputs, bit 5
     * (motor) low; 3, 6 and 7 are unconnected and keep the level last
     * driven for a while (a real C64 loses it in about a second). The
     * C128 has CAPS LOCK on bit 6 (low: locked).
     */
    cbm.port_float = (cbm.port_float & ~cbm.port_ddr) | (cbm.port_data & cbm.port_ddr & 0xC8);
    uint8_t in = 0x17 | cbm.port_float;
    if (cbm.cfg.model == CBM_MODEL_C128) {
        in = (in & ~0x40) | (input_caps_lock() ? 0 : 0x40);
    }
    uint8_t v = (cbm.port_data & cbm.port_ddr) | (~cbm.port_ddr & in);
    if (cbm.cfg.model == CBM_MODEL_C128) {
        /* (as VICE has it) CAPS LOCK picks the character set: the second
         * 4 KB while it's down. In C128 mode bit 0 (low) gives the CPU the
         * second colour RAM bank, bit 1 the VIC; bit 2 low puts the
         * character ROM at $1000 in all four VIC banks, high nowhere.
         */
        const uint8_t *chars = cbm.cfg.chargen + ((v & 0x40) ? 0 : 0x1000);
        bool c128 = c128_mode();
        const uint8_t *vc = cbm.color_ram + (c128 && !(v & 0x02) ? 0x400 : 0);
        bool rom = !c128 || !(v & 0x04);
        cbm.cpu_color = cbm.color_ram + (c128 && !(v & 0x01) ? 0x400 : 0);
        cbm.chargen = chars;
        if (chars != vic_chargen || vc != vic_color || rom != vic_rom_chars || c128 != vic_chars_all_banks) {
            vic_catch_up();
            vic_chargen = chars;
            vic_color = vc;
            vic_rom_chars = rom;
            vic_chars_all_banks = c128;
            vic_mem_changed();
        }
    }
    mem_update();                       /* (C128: may move page 0) */
    cbm.zero_page[0] = cbm.port_ddr;
    cbm.zero_page[1] = v;
}

/* ---- I/O ---- */

static void update_events(void)
{
    s_next_event = cbm.cia1.next_event < cbm.cia2.next_event ? cbm.cia1.next_event : cbm.cia2.next_event;
    m6502_lower_until(&cbm.cpu, cbm_cpu_time(s_next_event));   /* the CPU stops there (from an I/O access) */
}

/* C128 $D030: 2 MHz from the next instruction (the switch happens between
 * runs: inside one the counter is the run's)
 */
void cbm_fast_request(bool fast)
{
    if (cbm.cfg.model == CBM_MODEL_C128 && fast != cbm.fast) {
        cbm.fast_request = fast;
        m6502_lower_until(&cbm.cpu, cbm.cpu.cycles);
    }
}

static void apply_fast_request(void)
{
    bool fast = cbm.fast_request > 0;
    cbm.fast_request = -1;
    if (fast == cbm.fast) {
        return;
    }
    m6502_t *c = &cbm.cpu;
    cbm_sync_clock();
    if (fast) {
        cbm.fast_cpu0 = c->cycles;
        cbm.fast_clock0 = cbm.clock;
        m6502_set_stall_from(c, UINT64_MAX);        /* the VIC can't stop it */
    } else {
        /* back to 1 MHz: the CPU's counter steps back to the clock */
        uint64_t d = c->cycles - cbm.clock;
        c->cycles -= d;
        c->poll_cycle -= d;
        c->hijack_cycle -= d;
        if (c->nmi_pending && c->nmi_cycle) {
            c->nmi_cycle -= d;
        }
    }
    cbm.fast = fast;
}

/* After the Z80 had the bus (the clock went on without the 8502) */
void cbm_8502_clock_resume(void)
{
    if (cbm.fast) {
        cbm.fast_cpu0 = cbm.cpu.cycles;
        cbm.fast_clock0 = cbm.clock;
    } else if (cbm.cpu.cycles < cbm.clock) {
        cbm.cpu.cycles = cbm.clock;
    }
}

void cbm_events_changed(void)
{
    update_events();
}

uint8_t CBM_HOT cbm_io_read(uint16_t a)
{
    PROF_COUNT(cbm_prof_io);
    switch (a >> 8) {
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        return vic_read(a & 0x3F);
    case 0xD5: case 0xD6: case 0xD7:
        if (c128_mode()) {
            return (a >> 8) == 0xD5 ? c128_mmu_read(a) : (a >> 8) == 0xD6 ? vdc_read(a) : 0xFF;
        }
        __attribute__((fallthrough));   /* SID mirrors */
    case 0xD4:
        return sid_read(a & 0x1F);
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        return cbm.cpu_color[a & 0x3FF] | 0xF0;
    case 0xDC: {
        uint8_t v = cia_read(&cbm.cia1, a & 0x0F);
        if ((a & 0x0F) >= 4) {
            update_events();            /* (the ports don't touch the timers) */
        }
        return v;
    }
    case 0xDD: {
        if ((a & 0x0F) == 0) {
            drives_run();               /* the serial bus as the drives drive it now */
        }
        uint8_t v = cia_read(&cbm.cia2, a & 0x0F);
        if ((a & 0x0F) >= 4) {
            update_events();
        }
        return v;
    }
    default:
        return cart_io_read(a);
    }
}

void CBM_HOT cbm_io_write(uint16_t a, uint8_t v)
{
    switch (a >> 8) {
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        vic_write(a & 0x3F, v);
        break;
    case 0xD5: case 0xD6: case 0xD7:
        if (c128_mode()) {
            if ((a >> 8) == 0xD5) {
                c128_mmu_write(a, v);
            } else if ((a >> 8) == 0xD6) {
                vdc_write(a, v);
            }
            break;
        }
        __attribute__((fallthrough));
    case 0xD4:
        sid_write(a & 0x1F, v);
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        cbm.cpu_color[a & 0x3FF] = v & 0x0F;
        break;
    case 0xDC:
        cia_write(&cbm.cia1, a & 0x0F, v);
        if ((a & 0x0F) >= 4) {
            update_events();
        }
        break;
    case 0xDD:
        /* (the drives follow the bus from its timestamped history) */
        cia_write(&cbm.cia2, a & 0x0F, v);
        if ((a & 0x0F) >= 4) {
            update_events();
        }
        break;
    default:
        cart_io_write(a, v);
        break;
    }
}

static uint8_t CBM_HOT bus_read(void *ctx, uint16_t a)
{
    cbm_sync_clock();
    if (a >= 0xD000 && a < 0xE000) {
        return cbm_io_read(a);
    }
    if (a >= 0xFF00 && c128_mode()) {
        return c128_page_ff_read(a);
    }
    return 0xFF;                        /* open bus */
}

static void CBM_HOT bus_write(void *ctx, uint16_t a, uint8_t v)
{
    cbm_sync_clock();
    if (a < 0x100) {
        if (a == 0) {
            cbm.port_ddr = v;
            port_changed();
        } else if (a == 1) {
            cbm.port_data = v;
            port_changed();
        } else {
            cbm.zero_page[a] = v;
        }
        return;
    }
    if (a >= 0xD000 && a < 0xE000) {
        cbm_io_write(a, v);
        return;
    }
    if (a >= 0xFF00 && c128_mode()) {
        c128_page_ff_write(a, v);
        return;
    }
    /* ROM and unmapped areas: nothing (ROM areas write RAM, see map()) */
}

/* ---- The CIAs' wiring ---- */

static inline uint8_t outputs(const cia_t *c, int port)
{
    return port ? (c->prb | ~c->ddrb) : (c->pra | ~c->ddra);
}

/* CIA 1: keyboard rows on port A (out), columns on port B (in), joystick
 * 2 on port A, joystick 1 on port B
 */
static uint8_t cia1_in(cia_t *c, int port)
{
    if (port == 0) {
        return input_rows(outputs(c, 1)) & input_joystick(2);
    }
    uint8_t v = input_columns(outputs(c, 0)) & input_joystick(1);
    if (cbm.cfg.model == CBM_MODEL_C128) {
        v &= input_k_columns(vic_d02f);  /* the C128's keys on K0-K2 */
    }
    return v;
}

static void cia1_irq(cia_t *c, bool on)
{
    cbm.cpu.irq = on ? cbm.cpu.irq | IRQ_CIA1 : cbm.cpu.irq & ~IRQ_CIA1;
}

/* C128: CIA 1's shift register is fast serial (with SRQ as its clock) */
static void cia1_sdr(cia_t *c, uint8_t byte)
{
    if (c128_fast_serial_out()) {
        iec_fast_out(byte);
    }
}

/* CIA 2 port A: bits 0-1 VIC bank (inverted), 3 ATN out, 4 CLK out,
 * 5 DATA out (through inverters: 1 pulls the line low), 6 CLK in, 7 DATA
 * in (the line's level)
 */
static uint8_t cia2_in(cia_t *c, int port)
{
    if (port == 1) {
        return 0xFF;                    /* user port */
    }
    bool clk, data;
    iec_computer_in(&clk, &data);
    return 0x3F | (clk ? 0 : 0x40) | (data ? 0 : 0x80);
}

static void cia2_out(cia_t *c, int port)
{
    if (port != 0) {
        return;
    }
    uint8_t v = outputs(c, 0);
    vic_set_bank(~v & 3);
    iec_computer_out(v & 0x08, v & 0x10, v & 0x20);
}

static void cia2_irq(cia_t *c, bool on)
{
    if (on) {
        m6502_nmi_at(&cbm.cpu, cbm_cpu_time(c->irq_time));     /* NMI: an edge */
    }
}

/* ---- Running ---- */

/* The ROMs and the memory map for a C64 (or the C128's C64 mode) */
static void use_c64_roms(void)
{
    if (cbm.cfg.model == CBM_MODEL_C128) {
        cbm.basic = cbm.cfg.c64_rom;
        cbm.kernal = cbm.cfg.c64_rom + 0x2000;
        cbm.chargen = cbm.cfg.chargen;          /* the C64 set: the first 4 KB */
    } else {
        cbm.basic = cbm.cfg.basic;
        cbm.kernal = cbm.cfg.kernal;
        cbm.chargen = cbm.cfg.chargen;
    }
    vic_catch_up();
    vic_ram = cbm.cfg.ram;
    vic_chargen = cbm.chargen;
    vic_color = cbm.color_ram;
    vic_rom_chars = true;
    vic_chars_all_banks = false;
    vic_mem_changed();
    cbm.cpu_color = cbm.color_ram;
    cbm.zero_page = cbm.cfg.ram;
}

void c128_enter_c64_mode(void)
{
    use_c64_roms();
    port_changed();
}

void cbm_reset(void)
{
    m6502_t *c = &cbm.cpu;
    cbm.port_ddr = 0;
    cbm.port_data = 0;
    cbm.port_float = 0;
    use_c64_roms();
    if (cbm.cfg.model == CBM_MODEL_C128) {
        c128_reset();
        vdc_reset();
    }
    cart_reset();
    port_changed();
    cia_reset(&cbm.cia1);
    cia_reset(&cbm.cia2);
    vic_reset();
    sid_reset();
    input_reset();
    drives_reset();
    c->irq = 0;
    if (cbm.cfg.model != CBM_MODEL_C128) {
        m6502_reset(c);                 /* (the C128: when the Z80 hands over) */
    }
    update_events();
}

/* ---- The debugger (emudbg.h): the 6510 (8502), the C128's Z80, the drives' 6502s ---- */

static emudbg_cpu_t s_dbg_cpus[4];
static const char *const s_drive_names[CBM_DRIVES] = { "drive 8", "drive 9" };

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu, line %d, cycle %d", (unsigned long)cbm.frames, cbm.line,
             (int)(cbm.clock - cbm.line_start));
}

static emudbg_machine_t s_dbg = {
    .cpus = s_dbg_cpus,
    .where = dbg_where,
    .reset = cbm_reset,
    .screen = cbm_screen_mode,
    .render_line = cbm_render_line_rgb565,
    .type = cbm_type,
};

static void dbg_set_machine(void)
{
    bool c128 = cbm.cfg.model == CBM_MODEL_C128;
    int n = 0;
    s_dbg_cpus[n++] = (emudbg_cpu_t){ .name = c128 ? "8502" : "6510", .arch = &emudbg_arch_6502,
                                      .core = &cbm.cpu };
    if (c128) {
        s_dbg_cpus[n++] = (emudbg_cpu_t){ .name = "Z80", .arch = &emudbg_arch_z80,
                                          .peek = c128_z80_peek, .poke = c128_z80_poke };
    }
    for (int i = 0; i < CBM_DRIVES; i++) {
        if (drive_cpu(i)) {
            s_dbg_cpus[n++] = (emudbg_cpu_t){ .name = s_drive_names[i], .arch = &emudbg_arch_6502,
                                              .core = drive_cpu(i) };
        }
    }
    s_dbg.cpu_count = n;
    s_dbg.name = c128 ? "Commodore 128" : "Commodore 64";
    emudbg_set_machine(&s_dbg);
}

bool cbm_init(const cbm_config_t *config)
{
    if (config->ram == NULL || config->basic == NULL || config->kernal == NULL || config->chargen == NULL ||
        config->frame == NULL) {
        return false;
    }
    uint64_t cycles = cbm.clock;
    memset(&cbm, 0, sizeof(cbm));
    cbm.clock = cycles;
    cbm.fast_request = -1;
    cbm.cfg = *config;
    memset(s_open_bus, 0xFF, sizeof(s_open_bus));
    if (config->model == CBM_MODEL_C128 && (config->c64_rom == NULL || config->vdc_frame == NULL)) {
        return false;
    }
    /* RAM at power-on: blocks of $00 and $FF, as on many C64s */
    int ram_size = config->model == CBM_MODEL_C128 ? 0x20000 : 0x10000;
    for (int i = 0; i < ram_size; i++) {
        config->ram[i] = (i & 0x40) ? 0xFF : 0x00;
    }
    static const m6502_bus_t bus = { .read = bus_read, .write = bus_write, .stall = vic_cpu_stall };
    m6502_init(&cbm.cpu, &bus);
    cbm.cpu.cycles = cycles;
    cia_init(&cbm.cia1, &cbm.clock);
    cbm.cia1.port_in = cia1_in;
    cbm.cia1.irq_out = cia1_irq;
    if (config->model == CBM_MODEL_C128) {
        cbm.cia1.sdr_out = cia1_sdr;
    }
    cia_init(&cbm.cia2, &cbm.clock);
    cbm.cia2.port_in = cia2_in;
    cbm.cia2.port_out = cia2_out;
    cbm.cia2.irq_out = cia2_irq;
    vic_ram = config->ram;
    vic_chargen = config->chargen;
    vic_color = cbm.color_ram;
    vic_init();
    sid_init();
    if (config->model == CBM_MODEL_C128) {
        c128_init();
        vdc_init();
    }
    drives_init(config->drive_rom, config->model == CBM_MODEL_C128);
    cbm_reset();
    dbg_set_machine();
    return true;
}

/* The C128's 80 columns while 40/80 DISPLAY is down (it's two monitors) */
static bool showing_vdc(void)
{
    return c128_mode() && input_40_80();
}

void CBM_HOT cbm_run_frame(void)
{
    PROF_START();
    if (cbm.reset_pending) {
        cbm.reset_pending = false;
        cbm_reset();
    }
    input_frame();
    m6502_t *c = &cbm.cpu;
    uint64_t start = cbm.clock;
    static bool restore;
    if (input_restore() && !restore) {
        m6502_nmi(c);                   /* RESTORE: NMI on the key's edge */
    }
    restore = input_restore();
    for (int line = 0; line < CBM_LINES; line++) {
        cbm.line = line;
        cbm.line_start = start + (uint64_t)line * CBM_LINE_CYCLES;
        vic_line_start(line);
        uint64_t end = cbm.line_start + CBM_LINE_CYCLES;
        while (cbm.clock < end) {
            if (cbm.clock >= s_next_event) {
                cia_update(&cbm.cia1);
                cia_update(&cbm.cia2);
                update_events();
            }
            /* up to the line's end or the next CIA event */
            uint64_t until = s_next_event < end ? s_next_event : end;
            if (c128_z80_active()) {
                c128_z80_run(until);
                continue;
            }
            if (cbm.cfg.model == CBM_MODEL_C128) {
                c128_8502_start();
            }
            c->until = cbm_cpu_time(until);
            m6502_run(c);
            cbm_sync_clock();
            uint64_t poll = cbm.fast ? cbm.fast_clock0 + ((c->poll_cycle - cbm.fast_cpu0) >> 1) : c->poll_cycle;
            if (s_next_event <= poll) {
                /* an interrupt by the instruction's last cycle but one
                 * comes after it, not after the next one
                 */
                cia_update_to(&cbm.cia1, poll);
                cia_update_to(&cbm.cia2, poll);
                update_events();
                m6502_repoll(c);
            }
            if (cbm.fast_request >= 0) {
                apply_fast_request();
            }
        }
        {
            PROF_START();
            vic_line_end();
            PROF_ADD(cbm_prof_vic);
        }
        drives_run();
    }
    vic_frame_end();
    if (cbm.cfg.model == CBM_MODEL_C128) {
        vdc_frame_end(showing_vdc());
    }
    sid_frame_end();
    drives_frame_end();
    cia_tod_tick(&cbm.cia1);
    cia_tod_tick(&cbm.cia2);
    update_events();
    cbm.frames++;
    PROF_ADD(cbm_prof_frame);
#ifdef CBM_PROFILE
    {
        static uint32_t start_cycles, n;
        uint32_t now = esp_cpu_get_cycle_count();
        if (++n == 250) {
            CBM_LOG("per frame: %lu cycles, running %lu, drives %lu (%lu runs), VIC line ends %lu",
                    (unsigned long)((now - start_cycles) / 250), (unsigned long)(cbm_prof_frame / 250),
                    (unsigned long)(cbm_prof_drive / 250), (unsigned long)(cbm_prof_drive_steps / 250),
                    (unsigned long)(cbm_prof_vic / 250));
            CBM_LOG("  I/O reads %lu, drive I/O %lu, drives_run calls %lu", (unsigned long)(cbm_prof_io / 250),
                    (unsigned long)(cbm_prof_drive_io / 250), (unsigned long)(cbm_prof_drives_calls / 250));
            cbm_prof_drive = cbm_prof_vic = cbm_prof_frame = cbm_prof_drive_steps = 0;
            cbm_prof_io = cbm_prof_drive_io = cbm_prof_drives_calls = 0;
            n = 0;
            start_cycles = esp_cpu_get_cycle_count();
        }
    }
#endif
}

void cbm_get_status(cbm_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->frames = cbm.frames;
    s->pc = cbm.cpu.pc;
    s->port01 = cbm.port_data;
    s->raster = vic_raster();
    s->z80 = c128_z80_active();
    if (s->z80) {
        s->pc = c128_z80_pc();
    }
    s->fast = cbm.cfg.model == CBM_MODEL_C128 && (vic_d030 & 1);
    s->vdc = showing_vdc();
    drives_status(s);
}

cbm_model_t cbm_model(void)
{
    return cbm.cfg.model;
}

void cbm_screen_mode(int *width, int *height)
{
    bool vdc = showing_vdc();
    *width = vdc ? CBM_VDC_WIDTH : CBM_SCREEN_WIDTH;
    *height = vdc ? CBM_VDC_HEIGHT : CBM_SCREEN_HEIGHT;
}

void cbm_render_line_rgb565(int y, uint16_t *dst)
{
    if (showing_vdc()) {
        if (y >= 0 && y < CBM_VDC_HEIGHT) {
            memcpy(dst, cbm.cfg.vdc_frame + y * CBM_VDC_WIDTH, CBM_VDC_WIDTH * sizeof(uint16_t));
        }
    } else if (y >= 0 && y < CBM_SCREEN_HEIGHT) {
        memcpy(dst, cbm.cfg.frame + y * CBM_SCREEN_WIDTH, CBM_SCREEN_WIDTH * sizeof(uint16_t));
    }
}

uint32_t cbm_screen_signature(void)
{
    return showing_vdc() ? vdc_signature() * 2654435761u + 1 : vic_signature();
}

m6502_t *cbm_main_cpu(void)
{
    return &cbm.cpu;
}
