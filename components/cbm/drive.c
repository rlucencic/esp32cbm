/*
 * Floppy drives 8 and 9 on the serial bus: each a 6502 at 1 MHz with 2 KB
 * RAM, its DOS ROM, VIA 1 ($1800: the serial bus, ATN on CA1) and VIA 2
 * ($1C00: head stepper, motor, LED, write protect, density, SYNC on port
 * B; the GCR byte stream on port A, "byte ready" on CA1 and the CPU's SO
 * pin when CA2 enables it, write mode on CB2).
 *
 * The drives lag behind the computer and catch up (drives_run()) whenever
 * the computer touches the bus and at every raster line. The computer's
 * bus outputs are kept with their cycle times, so a drive sees each change
 * at its own moment; the computer sees the drives as they are once caught
 * up. The disk turns under the head at the speed the DOS selects (26-32
 * cycles a byte); a sync is a run of $FF bytes.
 *
 * The bus (open collector, true = pulled low): a drive's DATA output is
 * VIA 1 PB1, or the ATN acknowledge circuit (ATN low XOR ATNA, PB4);
 * CLK is PB3. Inputs through inverters: PB0 DATA, PB2 CLK, PB7 ATN.
 *
 * The 1571 (the C128's): 32 KB ROM at $8000, VIA 1 port A with the track 0
 * sensor (PA0, low), the fast serial direction (PA1), the side (PA2) and
 * 2 MHz (PA5); a 6526 at $4000 whose shift register does fast serial with
 * the C128's CIA 1 (a byte shifted out on one side arrives in the other's,
 * when that side listens); the WD1770 at $2000 (MFM: commands run and
 * find nothing, "record not found" after five turns of the disk, as on a
 * GCR disk). Double-sided disks (.D71).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cbm_internal.h"
#include "gcr.h"
#include "via.h"

#define DRIVE_HZ        1000000
#define HISTORY         64

typedef struct {
    bool present;
    int unit;
    m6502_t cpu;
    via_t via1, via2;
    uint8_t ram[2048];
    const uint8_t *rom;
    gcr_disk_t *disk;               /* in PSRAM: used per byte, not per access */
    bool inserted;
    /* the mechanism */
    int half_track;                 /* 0 = track 1 */
    uint8_t phase;                  /* stepper (VIA 2 PB0-1) */
    uint32_t pos;                   /* byte under the head */
    uint64_t next_byte;
    uint8_t prev;
    bool sync, motor, led;
    bool byte_ready;                /* the BYTE READY line (a 1571 reads it on VIA 1 PA7) */
    bool idle;                      /* in the DOS's idle loop: not run until the bus moves */
    /* the computer's bus outputs seen so far */
    uint32_t hist_pos;
    bool atn, clk, data;            /* from the computer, at this drive's time */
    uint8_t pb1;                    /* VIA 1 port B as it drives the bus */
    uint32_t reads, writes;
    /* the clock: drive cycles = base_d + speed * (computer cycles - base_c) * 1 MHz / 985248 Hz */
    uint64_t base_c, base_d;
    int speed;                      /* 1 or 2 (a 1571 at 2 MHz) */
    /* the 1571's */
    cia_t cia;
    int side;
    bool fast_out;                  /* fast serial direction: out */
    /* WD1770: status, track, sector, data; the command running until done_at */
    uint8_t wd[4];
    uint8_t wd_cmd;
    uint64_t wd_done_at;
} drive_t;

static drive_t s_drive[CBM_DRIVES];
static bool s_1571;
static uint8_t s_open[256];

/* The computer's outputs with their times (true = pulled low) */
static struct {
    uint64_t t;
    bool atn, clk, data;
    int16_t fast;                   /* a fast serial byte (C128), or -1 */
} s_hist[HISTORY];
static uint32_t s_hist_head;
static bool s_atn, s_clk, s_data;   /* the computer's now */

/* Computer cycles <-> drive cycles (985248 Hz vs 1 MHz) */
/* x * k / 2^32 with 32-bit multiplies (a 64-bit division is a slow library
 * call on the ESP32, and these come for every drive instruction while the
 * bus changes); off by a cycle in some 10 hours
 */
static inline uint64_t mul_frac(uint64_t x, uint32_t k)
{
    return (x >> 32) * k + (((x & 0xFFFFFFFF) * k) >> 32);
}

static inline uint64_t to_drive_rel(uint64_t c)
{
    return c + mul_frac(c, 64308030);   /* 14752 / 985248 */
}

static inline uint64_t to_computer_rel(uint64_t d)
{
    return d - mul_frac(d, 63359358);   /* 14752 / 1000000 */
}

/* Computer time c as drive d's cycles, and back */
static inline uint64_t to_drive(const drive_t *d, uint64_t c)
{
    uint64_t rel = c > d->base_c ? to_drive_rel(c - d->base_c) : 0;
    return d->base_d + (d->speed == 2 ? rel * 2 : rel);
}

static inline uint64_t to_computer(const drive_t *d, uint64_t t)
{
    uint64_t rel = t > d->base_d ? t - d->base_d : 0;
    return d->base_c + to_computer_rel(d->speed == 2 ? rel / 2 : rel);
}

/* The clock starts at the computer's now (at 1 MHz) */
static void clock_start(drive_t *d)
{
    d->base_c = cbm_now();
    d->base_d = d->cpu.cycles = to_drive_rel(d->base_c);
    d->speed = 1;
}

/* A 1571 switches between 1 and 2 MHz: the clock goes on from now, the
 * next disk byte as far away in time
 */
static void set_speed(drive_t *d, int speed)
{
    if (speed == d->speed) {
        return;
    }
#ifdef CBM_FAST_LOG
    CBM_LOG("drive %d: %d MHz (PC $%04X)", d->unit, speed, d->cpu.pc);
#endif
    uint64_t t = d->cpu.cycles;
    d->base_c = to_computer(d, t);
    d->base_d = t;
    if (d->next_byte > t) {
        d->next_byte = t + (speed == 2 ? (d->next_byte - t) * 2 : (d->next_byte - t) / 2);
        m6502_lower_until(&d->cpu, d->next_byte);
    }
    d->speed = speed;
}

/* ---- The bus ---- */

static inline bool drive_data_out(const drive_t *d, bool atn_low)
{
    return (d->pb1 & 0x02) || (((d->pb1 >> 4) & 1) != atn_low);
}

static inline bool drive_clk_out(const drive_t *d)
{
    return d->pb1 & 0x08;
}

void iec_computer_out(bool atn, bool clk, bool data)
{
    if (atn == s_atn && clk == s_clk && data == s_data) {
        return;
    }
    s_atn = atn;
    s_clk = clk;
    s_data = data;
    uint32_t i = s_hist_head % HISTORY;
    s_hist[i].t = cbm_now();
    s_hist[i].atn = atn;
    s_hist[i].clk = clk;
    s_hist[i].data = data;
    s_hist[i].fast = -1;
    s_hist_head++;
}

void iec_fast_out(uint8_t byte)
{
#ifdef CBM_FAST_LOG
    CBM_LOG("fast out $%02X at %llu", byte, (unsigned long long)cbm_now());
#endif
    if (!s_1571) {
        return;
    }
    uint32_t i = s_hist_head % HISTORY;
    s_hist[i].t = cbm_now();
    s_hist[i].atn = s_atn;
    s_hist[i].clk = s_clk;
    s_hist[i].data = s_data;
    s_hist[i].fast = byte;
    s_hist_head++;
}

void iec_computer_in(bool *clk, bool *data)
{
    bool c = s_clk, dt = s_data;
    for (int i = 0; i < CBM_DRIVES; i++) {
        const drive_t *d = &s_drive[i];
        if (d->present) {
            c |= drive_clk_out(d);
            dt |= drive_data_out(d, s_atn);
        }
    }
    *clk = c;
    *data = dt;
}

/* The bus as drive d sees it at its time */
static void drive_bus(const drive_t *d, bool *atn, bool *clk, bool *data)
{
    bool c = d->clk, dt = d->data;
    for (int i = 0; i < CBM_DRIVES; i++) {
        const drive_t *o = &s_drive[i];
        if (o->present) {
            c |= drive_clk_out(o);
            dt |= drive_data_out(o, d->atn);
        }
    }
    *atn = d->atn;
    *clk = c;
    *data = dt;
}

/* The computer's changes up to drive time `now` */
static void follow_computer(drive_t *d)
{
    uint64_t now = to_computer(d, d->cpu.cycles);
    while (d->hist_pos != s_hist_head) {
        const typeof(s_hist[0]) *h = &s_hist[d->hist_pos % HISTORY];
        if (h->t > now) {
            break;
        }
        if (s_hist_head - d->hist_pos > HISTORY) {
            d->hist_pos = s_hist_head - HISTORY;        /* fell too far behind */
            continue;
        }
        d->atn = h->atn;
        d->clk = h->clk;
        d->data = h->data;
        d->hist_pos++;
        via_ca1(&d->via1, d->atn);                      /* ATN in (inverted) */
#ifdef CBM_FAST_LOG
        if (h->fast >= 0) {
            CBM_LOG("drive %d: fast byte $%02X (%s, SP %s)", d->unit, h->fast, d->fast_out ? "drive sends" : "drive listens",
                    (d->cia.cr[0] & 0x40) ? "out" : "in");
        }
#endif
        if (h->fast >= 0 && s_1571 && !d->fast_out) {
            cia_sdr_in(&d->cia, (uint8_t)h->fast);      /* a fast serial byte */
        }
    }
}

/* ---- VIA 1: the bus ---- */

static uint8_t via1_in(via_t *v, int port)
{
    drive_t *d = v->ctx;
    if (port == 0) {
        /* 1571: PA0 the track 0 sensor, PA7 BYTE READY (both low) */
        return s_1571 ? (0x7E | (d->half_track == 0 ? 0 : 1) | (d->byte_ready ? 0 : 0x80)) : 0xFF;
    }
    bool atn, clk, data;
    drive_bus(d, &atn, &clk, &data);
    return (data ? 0x01 : 0) | (clk ? 0x04 : 0) | (atn ? 0x80 : 0) | ((d->unit - 8) << 5) | 0x1A;
}

static void disk_bytes(drive_t *d);

static void via1_out(via_t *v, int port)
{
    drive_t *d = v->ctx;
    if (port == 1) {
        d->pb1 = via_port_out(v, 1);
    } else if (s_1571) {
        /* 1571: PA1 fast serial out, PA2 the side, PA5 2 MHz */
        uint8_t pa = via_port_out(v, 0);
        d->fast_out = pa & 0x02;
        int side = (pa & 0x04) ? 1 : 0;
        if (side != d->side) {
#ifdef CBM_FAST_LOG
            CBM_LOG("drive %d: side %d (PC $%04X, half track %d)", d->unit, side, d->cpu.pc, d->half_track);
#endif
            if (d->motor && d->cpu.cycles >= d->next_byte) {
                disk_bytes(d);          /* (the bytes under the old head first) */
            }
            d->side = side;
        }
        set_speed(d, (pa & 0x20) ? 2 : 1);
    }
}

/* ---- VIA 2: the mechanism ---- */

/* The half track under the head, with the side */
static inline int track_index(const drive_t *d, int half)
{
    return d->side * GCR_HALF_TRACKS + half;
}

static void step_to(drive_t *d, int half)
{
    if (half < 0) {
        half = 0;
    }
    if (half >= GCR_HALF_TRACKS) {
        half = GCR_HALF_TRACKS - 1;
    }
    if (half == d->half_track) {
        return;
    }
    uint16_t old = d->disk->len[track_index(d, d->half_track)], new = d->disk->len[track_index(d, half)];
    if (old && new) {
        d->pos = d->pos * new / old;
    } else {
        d->pos = 0;
    }
    d->half_track = half;
}

/* The CPU must see each byte on time: "byte ready" on SO, or its interrupt.
 * Then the drive's runs stop at each byte. Otherwise (most of the time the
 * motor runs) the bytes are caught up when the CPU looks at VIA 2 (not
 * with SO on: from inside m6502_run(), setting V would be lost).
 */
static inline bool byte_stops(const drive_t *d)
{
    return via_ca2(&d->via2) || (d->via2.ier & 0x02);
}

static void byte_stops_changed(drive_t *d);

/* The CPU waits for the next byte in a loop: BVC * (SO), or BIT $180F /
 * BMI (a 1571's BYTE READY): nothing to run until it comes
 */
static bool waiting_for_byte(const drive_t *d)
{
    const m6502_t *c = &d->cpu;
    uint16_t pc = c->pc;
    uint8_t op = m6502_peek(c, pc);
    if (op == 0x50) {
        return m6502_peek(c, pc + 1) == 0xFE && !(c->p & M6502_V);
    }
    return op == 0x2C && s_1571 && m6502_peek(c, pc + 1) == 0x0F && m6502_peek(c, pc + 2) == 0x18 &&
           m6502_peek(c, pc + 3) == 0x30 && m6502_peek(c, pc + 4) == 0xFB && !d->byte_ready;
}

static void via2_out(via_t *v, int port)
{
    drive_t *d = v->ctx;
    byte_stops_changed(d);
    if (port != 1) {
        return;
    }
    uint8_t pb = via_port_out(v, 1);
    uint8_t phase = pb & 3;
    int diff = (phase - d->phase) & 3;
    if (diff == 1) {
        step_to(d, d->half_track + 1);
    } else if (diff == 3) {
        step_to(d, d->half_track - 1);
    }
    d->phase = phase;
    bool motor = pb & 0x04;
    if (motor && !d->motor) {
        d->next_byte = d->cpu.cycles + 32 * d->speed;
        m6502_lower_until(&d->cpu, d->next_byte);
    }
    d->motor = motor;
    d->led = pb & 0x08;
}

/* SO or the byte interrupt may have come on (from a VIA 2 write) */
static void byte_stops_changed(drive_t *d)
{
    if (d->motor && byte_stops(d)) {
        m6502_lower_until(&d->cpu, d->next_byte);
    }
}

static uint8_t via2_in(via_t *v, int port)
{
    drive_t *d = v->ctx;
    if (port == 0) {
        return d->prev;                 /* the last byte read */
    }
    bool wp = !d->inserted || d->disk->wp;
    return (d->sync ? 0 : 0x80) | (wp ? 0 : 0x10) | 0x6F;
}

static void drive_irq(via_t *v, bool on)
{
    drive_t *d = v->ctx;
    uint32_t bit = v == &d->via1 ? 1 : 2;
    d->cpu.irq = on ? d->cpu.irq | bit : d->cpu.irq & ~bit;
}

/* ---- The 1571's WD1770 ---- */

enum { WD_BUSY = 0x01, WD_TRACK0 = 0x04, WD_RNF = 0x10, WD_SPUN_UP = 0x20, WD_MOTOR = 0x80 };

/* Microseconds as this drive's cycles */
static inline uint64_t us(const drive_t *d, uint32_t n)
{
    return (uint64_t)n * d->speed;
}

static void wd_finish(drive_t *d)
{
    uint8_t cmd = d->wd_cmd;
    if (!(cmd & 0x80)) {
        /* type I: restore, seek, step (the 1571 moves the head itself:
         * only the track register)
         */
        if ((cmd & 0xF0) == 0x00) {
            d->wd[1] = 0;
        } else if ((cmd & 0xF0) == 0x10) {
            d->wd[1] = d->wd[3];
        } else if (cmd & 0x10) {
            d->wd[1] += (cmd & 0x60) == 0x60 ? -1 : (cmd & 0x60) == 0x40 ? 1 : 0;
        }
        d->wd[0] = WD_MOTOR | WD_SPUN_UP | (d->half_track == 0 ? WD_TRACK0 : 0);
    } else {
        d->wd[0] = WD_MOTOR | WD_RNF;   /* no MFM sectors */
    }
    d->wd_done_at = 0;
}

static uint8_t wd_read(drive_t *d, int reg)
{
    if (reg == 0 && d->wd_done_at && d->cpu.cycles >= d->wd_done_at) {
        wd_finish(d);
    }
    return d->wd[reg];
}

static void wd_write(drive_t *d, int reg, uint8_t v)
{
    if (reg != 0) {
        d->wd[reg] = v;
        return;
    }
    if ((v & 0xF0) == 0xD0) {
        d->wd[0] &= ~WD_BUSY;           /* force interrupt */
        d->wd_done_at = 0;
        return;
    }
    d->wd_cmd = v;
    d->wd[0] = WD_BUSY | WD_MOTOR;
    /* type I: some milliseconds; the others look for a sector for five
     * turns (200 ms each)
     */
    d->wd_done_at = d->cpu.cycles + ((v & 0x80) ? us(d, 1000000) : us(d, 10000));
}

/* ---- The 1571's CIA: fast serial ---- */

static void cia_irq(cia_t *c, bool on)
{
    drive_t *d = c->ctx;
    d->cpu.irq = on ? d->cpu.irq | 4 : d->cpu.irq & ~4;
}

/* A byte shifted out: to the C128's CIA 1, if the drive sends */
static void cia_sdr(cia_t *c, uint8_t byte)
{
    drive_t *d = c->ctx;
#ifdef CBM_FAST_LOG
    CBM_LOG("drive %d: shifted out $%02X (%s)", d->unit, byte, d->fast_out ? "sends" : "not sending");
#endif
    if (d->fast_out) {
        c128_fast_in(byte);
    }
}

/* The bytes that passed under the head by now */
static void disk_bytes(drive_t *d)
{
    uint8_t pb = via_port_out(&d->via2, 1);
    int cycles_per_byte = (32 - 2 * ((pb >> 5) & 3)) * d->speed;
    int ht = track_index(d, d->half_track);
    uint8_t *t = d->inserted ? d->disk->track[ht] : NULL;
    uint16_t len = d->inserted ? d->disk->len[ht] : 0;
    bool writing = !via_cb2(&d->via2);
    while (d->next_byte <= d->cpu.cycles) {
        uint8_t b = 0;
        if (t && len) {
            if (d->pos >= len) {
                d->pos = 0;
            }
            if (writing) {
                uint8_t v = via_port_out(&d->via2, 0);
                if (t[d->pos] != v) {
                    t[d->pos] = v;
                    d->disk->dirty[ht] = true;
                }
                b = v;
            } else {
                b = t[d->pos];
            }
            d->pos++;
        }
        bool sync = !writing && b == 0xFF && d->prev == 0xFF;
        d->sync = sync;
        d->prev = b;
        if (!sync) {
            via_latch_a(&d->via2, b);
            if (via_ca2(&d->via2)) {
                m6502_set_overflow(&d->cpu);    /* byte ready on SO */
                d->byte_ready = true;
            }
            via_ca1(&d->via2, false);
            via_ca1(&d->via2, true);
        }
        d->next_byte += cycles_per_byte;
    }
}

/* ---- The drive's bus ---- */

static uint8_t drive_read(void *ctx, uint16_t a)
{
    PROF_COUNT(cbm_prof_drive_io);
    drive_t *d = ctx;
    if ((a & 0xFC00) == 0x1800) {
        return via_read(&d->via1, a & 0x0F);
    }
    if (s_1571 && (a & 0xE000) == 0x2000) {
        return wd_read(d, a & 3);
    }
    if (s_1571 && (a & 0xC000) == 0x4000) {
        return cia_read(&d->cia, a & 0x0F);
    }
    if ((a & 0xFC00) == 0x1C00) {
        if (d->motor && d->cpu.cycles >= d->next_byte && !byte_stops(d)) {
            disk_bytes(d);              /* (caught up lazily) */
        }
        if ((a & 0x0F) <= 1 || (a & 0x0F) == 0x0F) {
            d->byte_ready = false;      /* (VICE: reading a port clears it) */
        }
        return via_read(&d->via2, a & 0x0F);
    }
    return 0xFF;
}

static void drive_write(void *ctx, uint16_t a, uint8_t v)
{
    drive_t *d = ctx;
    if (s_1571 && (a & 0xE000) == 0x2000) {
        wd_write(d, a & 3, v);
    } else if (s_1571 && (a & 0xC000) == 0x4000) {
        cia_write(&d->cia, a & 0x0F, v);
    } else if ((a & 0xFC00) == 0x1800) {
        via_write(&d->via1, a & 0x0F, v);
    } else if ((a & 0xFC00) == 0x1C00) {
        if (d->motor && d->cpu.cycles >= d->next_byte && !byte_stops(d)) {
            disk_bytes(d);
        }
        if ((a & 0x0F) <= 1 || (a & 0x0F) == 0x0F) {
            d->byte_ready = false;
        }
        via_write(&d->via2, a & 0x0F, v);
        byte_stops_changed(d);          /* (an IER write) */
    }
}

static void map_drive(drive_t *d)
{
    m6502_t *c = &d->cpu;
    for (int p = 0; p < 256; p++) {
        c->rd[p] = s_open;
        c->wr[p] = NULL;
    }
    for (int p = 0; p < 0x10; p++) {
        c->rd[p] = c->wr[p] = d->ram + (p & 7) * 256;   /* 2 KB, mirrored */
    }
    for (int p = 0x18; p < 0x20; p++) {
        c->rd[p] = NULL;
    }
    if (s_1571) {
        for (int p = 0x20; p < 0x80; p++) {
            c->rd[p] = NULL;            /* WD1770, CIA */
        }
        for (int p = 0x80; p < 0x100; p++) {
            c->rd[p] = (uint8_t *)d->rom + (p - 0x80) * 256;   /* 32 KB at $8000 */
        }
        return;
    }
    for (int p = 0x80; p < 0x100; p++) {
        c->rd[p] = (uint8_t *)d->rom + ((p * 256) & 0x3FFF);    /* 16 KB at $C000, mirrored at $8000 */
    }
}

/* ---- Running ---- */

void drives_init(const uint8_t *rom, bool is_1571)
{
    memset(s_open, 0xFF, sizeof(s_open));
    s_1571 = is_1571;
    for (int i = 0; i < CBM_DRIVES; i++) {
        drive_t *d = &s_drive[i];
        gcr_disk_t *disk = d->disk;
        bool inserted = d->inserted;
        memset(d, 0, sizeof(*d));
        d->disk = disk ? disk : cbm_calloc(1, sizeof(gcr_disk_t));
        d->inserted = inserted;
        d->present = rom != NULL;
        d->unit = 8 + i;
        d->rom = rom;
        static const m6502_bus_t bus = { .read = drive_read, .write = drive_write };
        m6502_init(&d->cpu, &bus);
        d->cpu.stop_on_spin = true;     /* (waiting_for_byte()) */
        d->cpu.bus.ctx = d;
        clock_start(d);
        via_init(&d->via1, &d->cpu.cycles);
        via_init(&d->via2, &d->cpu.cycles);
        d->via1.cpu = d->via2.cpu = &d->cpu;
        d->via1.ctx = d->via2.ctx = d;
        d->via1.port_in = via1_in;
        d->via2.port_in = via2_in;
        d->via1.port_out = via1_out;
        d->via2.port_out = via2_out;
        d->via1.irq_out = d->via2.irq_out = drive_irq;
        cia_init(&d->cia, &d->cpu.cycles);
        d->cia.cpu = &d->cpu;
        d->cia.ctx = d;
        d->cia.irq_out = cia_irq;
        d->cia.sdr_out = cia_sdr;
        d->half_track = 34;             /* track 18 */
        if (d->present) {
            map_drive(d);
        }
    }
}

void drives_reset(void)
{
    s_atn = s_clk = s_data = false;
    for (int i = 0; i < CBM_DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (!d->present) {
            continue;
        }
        d->hist_pos = s_hist_head;
        d->atn = d->clk = d->data = false;
        clock_start(d);
        d->side = 0;
        d->fast_out = false;
        via_reset(&d->via1);
        via_reset(&d->via2);
        cia_reset(&d->cia);
        memset(d->ram, 0, sizeof(d->ram));
        d->cpu.irq = 0;
        m6502_reset(&d->cpu);
    }
}

m6502_t *drive_cpu(int i)
{
    return i >= 0 && i < CBM_DRIVES && s_drive[i].present ? &s_drive[i].cpu : NULL;
}

bool drives_present(void)
{
    return s_drive[0].present;
}

/* The 1541's idle loop ends with JMP $EBFF at $EC9B ($7C: ATN pending),
 * the 1571's too. Reaching it with the motor off, the drive has nothing to
 * do until the computer moves the bus (VICE's "trap idle"): its clock just
 * follows. With the motor on, until its next timer interrupt.
 */
#define IDLE_PC     0xEC9B

void CBM_HOT drives_run(void)
{
    PROF_START();
    PROF_COUNT(cbm_prof_drives_calls);
    uint64_t now = cbm_now();
    for (int i = 0; i < CBM_DRIVES; i++) {
        drive_t *d = &s_drive[i];
        m6502_t *c = &d->cpu;
        uint64_t target = to_drive(d, now);
        if (!d->present || c->cycles >= target) {
            continue;                   /* (up to date: back-to-back bus reads) */
        }
        if (d->idle) {
            if (d->hist_pos == s_hist_head) {
                if (c->cycles < target) {
                    c->cycles = target;
                }
                continue;
            }
            /* woken by the computer: from the moment it moved */
            uint64_t t = to_drive(d, s_hist[d->hist_pos % HISTORY].t);
            if (t > c->cycles) {
                c->cycles = t;
            }
            d->idle = false;
        }
        while (c->cycles < target) {
            if (c->pc == IDLE_PC && !d->ram[0x7C] && !d->atn && d->hist_pos == s_hist_head) {
                if (!d->motor) {
                    d->idle = true;
                    c->cycles = target;
                    break;
                }
                /* The motor still turns: the DOS's work is in its timer
                 * interrupt (the job loop, the motor's run-out); on to it
                 */
                uint64_t u = target, t;
                t = via_next_irq(&d->via1);
                u = t < u ? t : u;
                t = via_next_irq(&d->via2);
                u = t < u ? t : u;
                if (s_1571 && (d->cia.imr & 0x1F)) {
                    u = d->cia.next_event < u ? d->cia.next_event : u;
                }
                if (u > c->cycles) {
                    c->cycles = u;
                }
                if (c->cycles >= target) {
                    break;
                }
            }
            if (d->hist_pos != s_hist_head) {
                follow_computer(d);
            }
            if (d->motor && c->cycles < d->next_byte && byte_stops(d) && waiting_for_byte(d)) {
                c->cycles = d->next_byte < target ? d->next_byte : target;
                if (c->cycles >= target) {
                    break;
                }
            }
            if (d->motor && c->cycles >= d->next_byte) {
                disk_bytes(d);
            }
            if (c->cycles >= d->via1.next_event) {
                via_update(&d->via1);
            }
            if (c->cycles >= d->via2.next_event) {
                via_update(&d->via2);
            }
            if (s_1571 && c->cycles >= d->cia.next_event) {
                cia_update(&d->cia);
            }
            /* run up to the next of those (the VIAs lower `until` when
             * their timers change), stopping at the idle loop
             */
            uint64_t u = target;
            if (d->motor && d->next_byte < u && byte_stops(d)) {
                u = d->next_byte;
            }
            if (d->via1.next_event < u) {
                u = d->via1.next_event;
            }
            if (d->via2.next_event < u) {
                u = d->via2.next_event;
            }
            if (s_1571 && d->cia.next_event < u) {
                u = d->cia.next_event;
            }
            if (d->hist_pos != s_hist_head) {
                uint64_t h = to_drive(d, s_hist[d->hist_pos % HISTORY].t);
                if (h < u) {
                    u = h;
                }
            }
            c->until = u > c->cycles ? u : c->cycles + 1;
            c->stop_pc = IDLE_PC;
            PROF_COUNT(cbm_prof_drive_steps);
            m6502_run(c);
        }
    }
    PROF_ADD(cbm_prof_drive);
}

void drives_frame_end(void)
{
}

/* ---- Disks ---- */

bool cbm_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= CBM_DRIVES) {
        return false;
    }
    drive_t *d = &s_drive[drive];
    if (d->disk == NULL) {
        d->disk = cbm_calloc(1, sizeof(gcr_disk_t));
    }
    gcr_disk_t *disk = cbm_malloc(sizeof(gcr_disk_t));
    if (d->disk == NULL || disk == NULL || !gcr_load(disk, path)) {
        free(disk);
        return false;
    }
    cbm_disk_eject(drive);
    *d->disk = *disk;
    free(disk);
    d->inserted = true;
    return true;
}

void cbm_disk_eject(int drive)
{
    if (drive < 0 || drive >= CBM_DRIVES || !s_drive[drive].inserted) {
        return;
    }
    drive_t *d = &s_drive[drive];
    if (!gcr_flush(d->disk, &d->writes)) {
        CBM_LOG("can't write %s", d->disk->path);
    }
    gcr_free(d->disk);
    d->inserted = false;
}

const char *cbm_disk_path(int drive)
{
    return drive >= 0 && drive < CBM_DRIVES && s_drive[drive].inserted ? s_drive[drive].disk->path : NULL;
}

bool cbm_disk_write_protected(int drive)
{
    return drive >= 0 && drive < CBM_DRIVES && s_drive[drive].inserted && s_drive[drive].disk->wp;
}

bool cbm_disk_dirty(void)
{
    for (int i = 0; i < CBM_DRIVES; i++) {
        if (s_drive[i].inserted && gcr_dirty(s_drive[i].disk)) {
            return true;
        }
    }
    return false;
}

void cbm_disk_flush(void)
{
    for (int i = 0; i < CBM_DRIVES; i++) {
        drive_t *d = &s_drive[i];
        if (d->inserted && !gcr_flush(d->disk, &d->writes)) {
            CBM_LOG("can't write %s", d->disk->path);
        }
    }
}

bool cbm_disk_create(const char *path, uint32_t bytes)
{
    if (bytes != CBM_D64_BYTES && bytes != CBM_D71_BYTES) {
        return false;
    }
    return gcr_create(path, bytes == CBM_D71_BYTES, "BLANK");
}

void cbm_disk_status(cbm_disk_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->light = -1;
    for (int i = 0; i < CBM_DRIVES; i++) {
        const drive_t *d = &s_drive[i];
        if (d->led && s->light < 0) {
            s->light = i;
        }
        s->writes[i] = d->writes;
        s->track[i] = d->half_track / 2 + 1 + d->side * 35;
        s->motor[i] = d->motor;
    }
}

void drives_status(cbm_status_t *s)
{
    for (int i = 0; i < CBM_DRIVES; i++) {
        s->drive_led[i] = s_drive[i].led;
        s->drive_motor[i] = s_drive[i].motor;
        s->drive_track[i] = s_drive[i].half_track / 2 + 1;
        s->drive_pc[i] = s_drive[i].cpu.pc;
    }
}
