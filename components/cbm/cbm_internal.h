/*
 * Between the Commodore core's modules.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cbm.h"
#include "m6502.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define CBM_LOG(fmt, ...)   ESP_LOGI("cbm", fmt, ##__VA_ARGS__)
/* The core's buffers in PSRAM: plain malloc() serves small blocks from
 * internal RAM, which WiFi, Bluetooth and the LCD need
 */
#define cbm_malloc(n)       heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define cbm_calloc(n, s)    heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define CBM_LOG(fmt, ...)   printf("cbm: " fmt "\n", ##__VA_ARGS__)
#define cbm_malloc(n)       malloc(n)
#define cbm_calloc(n, s)    calloc((n), (s))
#endif

#define CBM_HOT             M6502_HOT

/* -DCBM_PROFILE: where the time goes, logged every 5 s */
#ifdef CBM_PROFILE
#include "esp_cpu.h"
extern uint32_t cbm_prof_drive, cbm_prof_vic, cbm_prof_frame, cbm_prof_drive_steps;
extern uint32_t cbm_prof_io, cbm_prof_drive_io, cbm_prof_drives_calls;
#define PROF_START()    uint32_t prof_t0 = esp_cpu_get_cycle_count()
#define PROF_ADD(v)     (v += esp_cpu_get_cycle_count() - prof_t0)
#define PROF_COUNT(v)   (v++)
#else
#define PROF_START()
#define PROF_ADD(v)
#define PROF_COUNT(v)
#endif

/* IRQ sources on the CPU's irq line (bits) */
enum { IRQ_VIC = 0x01, IRQ_CIA1 = 0x02, IRQ_CART = 0x04 };

/* ---- cia.c: 6526 ---- */

typedef struct {
    uint16_t counter, latch;
    uint64_t last;                      /* the counter's value is at this cycle (may be ahead) */
    uint64_t load_at;                   /* a force load shows from then on, */
    uint16_t before;                    /* this until then (still counting: before_counts) */
    bool before_counts;
    uint64_t zero_at;                   /* it read 0 then (and underflowed) */
    bool counting;                      /* START as the counter sees it (three cycles late) */
    bool toggle;                        /* the PB6/PB7 toggle output */
    uint64_t load_underflow_at;         /* the old count's underflow before a load shows (0: none) */
    bool oneshot_before;                /* the one-shot bit before the control write at */
    uint64_t control_at;
    uint64_t stop_at;                   /* a stop: counts up to this cycle (0: none) */
} cia_timer_t;

typedef struct cia {
    uint8_t pra, prb, ddra, ddrb;       /* output registers */
    cia_timer_t t[2];                   /* A, B */
    uint64_t b_events[4];               /* A underflows that B is still to count */
    int b_event_n;
    bool b_event_checked;               /* the first one's zero check is done */
    uint8_t cr[2];                      /* CRA, CRB */
    uint8_t icr, imr;                   /* interrupt flags, mask */
    bool irq;
    uint8_t tod[4], alarm[4], latch[4]; /* tenths, s, min, h (BCD, h bit 7 PM) */
    bool tod_latched, tod_stopped;
    uint8_t tod_ticks;                  /* 50 Hz ticks of the current tenth */
    uint8_t sdr;
    int sdr_bits;                       /* serial output: bits still to shift */
    uint64_t irq_at;                    /* the IRQ line follows the flags then */
    uint64_t irq_time;                  /* when it last went on */
    uint64_t next_event;                /* the next interrupt (or shift register byte) */
    const uint64_t *now;                /* its CPU's cycle counter */
    struct m6502 *cpu;                  /* (a drive's) its CPU stops at next_event */
    /* The machine's side */
    uint8_t (*port_in)(struct cia *cia, int port);      /* pins as driven from outside (0xFF: none) */
    void (*port_out)(struct cia *cia, int port);        /* output changed */
    void (*irq_out)(struct cia *cia, bool on);
    void (*sdr_out)(struct cia *cia, uint8_t byte);     /* a byte shifted out (1571 fast serial) */
    void *ctx;
} cia_t;

void cia_init(cia_t *cia, const uint64_t *now);
void cia_reset(cia_t *cia);
uint8_t cia_read(cia_t *cia, uint8_t reg);
void cia_write(cia_t *cia, uint8_t reg, uint8_t v);
void cia_update(cia_t *cia);            /* timers up to *now */
void cia_update_to(cia_t *cia, uint64_t now);
void cia_tod_tick(cia_t *cia);          /* 50 Hz */
void cia_sdr_in(cia_t *cia, uint8_t byte);  /* a byte shifted in */
uint8_t cia_port_value(cia_t *cia, int port);   /* what the pins read */

/* ---- vic.c: VIC-II (6569 PAL) ---- */

void vic_init(void);
void vic_reset(void);
uint8_t vic_read(uint8_t reg);
void vic_write(uint8_t reg, uint8_t v);
void vic_line_start(int line);          /* raster IRQ, bad line, sprite DMA, stalls */
void vic_catch_up(void);                /* draw what the beam passed by now */
void vic_line_end(void);
void vic_frame_end(void);
uint16_t vic_raster(void);
void vic_set_bank(uint8_t bank);        /* CIA2 port A bits 0-1, inverted */
/* The C128's VIC-IIe: extra keyboard lines and 2 MHz/test bits at $D02F/$D030 */
extern uint8_t vic_d02f, vic_d030;
/* Where the VIC reads its 16 KB (and the character ROM in banks 0 and 2) */
extern uint8_t *vic_ram;                /* bank base = vic_ram + bank * 0x4000 */
extern const uint8_t *vic_chargen;
extern const uint8_t *vic_color;        /* 1 KB of colour RAM */
extern bool vic_rom_chars;              /* the character ROM in banks 0 and 2 (C128: $01 bit 2) */
extern bool vic_chars_all_banks;        /* C128 mode: in all four */
void vic_mem_changed(void);             /* one of those changed (vic_catch_up() first) */
void vic_cpu_stall(void *ctx, m6502_t *cpu);
uint32_t vic_signature(void);

/* ---- vdc.c: the C128's 80 columns ---- */

void vdc_init(void);
void vdc_reset(void);
uint8_t vdc_read(uint16_t a);           /* $D600/$D601 */
void vdc_write(uint16_t a, uint8_t v);
void vdc_frame_end(bool shown);
uint32_t vdc_signature(void);

/* ---- sid.c ---- */

void sid_init(void);
void sid_reset(void);
uint8_t sid_read(uint8_t reg);
void sid_write(uint8_t reg, uint8_t v);
void sid_frame_end(void);

/* ---- input.c ---- */

void input_reset(void);
void input_frame(void);
/* The keyboard as CIA1 sees it: columns read with rows selected (active
 * low), and rows read with columns driven
 */
uint8_t input_columns(uint8_t rows);
uint8_t input_rows(uint8_t columns);
uint8_t input_k_columns(uint8_t k_rows);    /* C128 K0-K2 */
uint8_t input_joystick(int port);           /* active-low bits 0-4 */
bool input_restore(void);                   /* RESTORE is down */
bool input_caps_lock(void);
bool input_40_80(void);

/* ---- iec.c, drive.c: the serial bus and the drives ---- */

/* The computer's bus outputs (true = pulled low) at the current cycle */
void iec_computer_out(bool atn, bool clk, bool data);
/* The bus as the computer sees it now (true = low) */
void iec_computer_in(bool *clk, bool *data);
void drives_init(const uint8_t *rom, bool is_1571);
void iec_fast_out(uint8_t byte);        /* C128: a byte shifted out on fast serial */
void drives_reset(void);
void drives_run(void);                  /* bring the drives up to the computer's time */
void drives_frame_end(void);
bool drives_present(void);
m6502_t *drive_cpu(int i);              /* the drive's CPU (NULL: no drive) */
void drives_status(cbm_status_t *status);

/* ---- space.c: free space on the disks (the web page) ---- */

struct gcr_disk;
/* Worked out again from the drive's disk (NULL: none); emulation stopped */
void space_update(int drive, const struct gcr_disk *disk);

/* ---- cart.c ---- */

extern bool cart_exrom, cart_game;      /* the lines as the PLA sees them (true = high) */
uint8_t cart_io_read(uint16_t addr);    /* $DE00-$DFFF */
void cart_io_write(uint16_t addr, uint8_t v);
const uint8_t *cart_roml(void);         /* 8 KB at $8000 (NULL: none) */
const uint8_t *cart_romh(void);         /* 8 KB at $A000 or $E000 */
void cart_reset(void);

/* ---- cbm.c: the machine ---- */

typedef struct {
    cbm_config_t cfg;
    m6502_t cpu;
    cia_t cia1, cia2;
    uint8_t port_ddr, port_data;        /* 6510 I/O port */
    uint8_t port_float;                 /* bits 3, 6, 7: last driven, kept as inputs */
    uint8_t *zero_page;                 /* where page 0 is (C128: its pointer) */
    uint8_t color_ram[2048];            /* C128: two banks */
    uint8_t *cpu_color;                 /* the 1 KB the CPU sees at $D800 */
    /* ROMs in use (the C128's C64 mode has its own) */
    const uint8_t *basic, *kernal, *chargen;
    int line;                           /* raster line being run */
    uint64_t line_start;                /* its first cycle */
    /* The machine's clock (1 MHz cycles: the CIAs, the VIC, the SID, the
     * drives). The CPU's counter is the same at 1 MHz; at the C128's 2 MHz
     * it counts twice as fast from fast_cpu0 / fast_clock0.
     */
    uint64_t clock;
    bool fast;
    int fast_request;                   /* $D030 bit 0 written: -1 none */
    uint64_t fast_cpu0, fast_clock0;
    uint32_t frames;
    bool reset_pending;
} cbm_t;

extern cbm_t cbm;

static inline uint64_t cbm_now(void)
{
    return cbm.clock;
}

static inline int cbm_line_cycle(void)
{
    return (int)(cbm.clock - cbm.line_start);           /* 0-62 */
}

/* The machine's clock from the CPU's counter (bus callbacks, after runs) */
static inline void cbm_sync_clock(void)
{
    cbm.clock = cbm.fast ? cbm.fast_clock0 + ((cbm.cpu.cycles - cbm.fast_cpu0) >> 1) : cbm.cpu.cycles;
}

/* A time of the machine's clock on the CPU's counter */
static inline uint64_t cbm_cpu_time(uint64_t t)
{
    if (!cbm.fast || t == UINT64_MAX) {
        return t;
    }
    return t > cbm.fast_clock0 ? cbm.fast_cpu0 + (t - cbm.fast_clock0) * 2 : cbm.fast_cpu0;
}

void cbm_fast_request(bool fast);       /* C128 $D030 bit 0 */

void mem_update(void);
uint8_t cbm_io_read(uint16_t a);        /* $D000-$DFFF */
void cbm_io_write(uint16_t a, uint8_t v);
void c128_enter_c64_mode(void);         /* (cbm.c) the MMU went into C64 mode */

/* ---- c128.c: the MMU, the Z80 ---- */

void c128_init(void);
void c128_reset(void);
uint8_t c128_z80_peek(void *ctx, uint32_t a);  /* memory as the Z80 sees it (the debugger) */
void c128_z80_poke(void *ctx, uint32_t a, uint8_t v);
void c128_mem_update(void);
bool c128_mode(void);                   /* a C128 in C128 mode */
bool c128_in_c64_mode(void);
bool c128_z80_active(void);
uint8_t c128_mmu_read(uint16_t a);      /* $D500-$D5FF */
void c128_mmu_write(uint16_t a, uint8_t v);
uint8_t c128_page_ff_read(uint16_t a);  /* $FF00-$FFFF */
void c128_page_ff_write(uint16_t a, uint8_t v);
void c128_z80_run(uint64_t until);
void c128_8502_start(void);
uint16_t c128_z80_pc(void);
bool c128_fast_serial_out(void);        /* MCR bit 3: the C128 sends */
void c128_fast_in(uint8_t byte);        /* a drive shifted a byte out */
void cbm_events_changed(void);          /* (cbm.c) a CIA's events moved */
void cbm_8502_clock_resume(void);       /* (cbm.c) after the Z80 */
