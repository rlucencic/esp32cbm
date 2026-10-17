/*
 * MOS 6526 CIA: two I/O ports, two 16-bit timers, the time-of-day clock,
 * the serial shift register, five interrupt sources.
 *
 * The timers run lazily: cia_update() counts them up to the CPU's cycle
 * counter (underflows, reloads, one-shot stops, timer B counting timer A,
 * the shift register clocked by timer A) and sets next_event to the next
 * underflow or IRQ, which the machine's loop checks between instructions.
 *
 * Timing as in the Lorenz tests (old 6526): a timer counts from the third
 * cycle after the write that starts or stops it; a force load shows on the
 * second and the counter holds it a cycle longer. The counter never reads
 * 0: where it would, it underflows and reloads, and holds that for a cycle
 * too (n + 1 cycles a turn). Timer B counting A's underflows checks for 0
 * a cycle after each and counts a cycle later. The IRQ line (and ICR bit
 * 7) follows a flag a cycle later.
 */
#include <string.h>
#include "cbm_internal.h"

enum { ICR_TA = 0x01, ICR_TB = 0x02, ICR_ALARM = 0x04, ICR_SDR = 0x08, ICR_FLAG = 0x10 };
enum { CR_START = 0x01, CR_PBON = 0x02, CR_TOGGLE = 0x04, CR_ONESHOT = 0x08, CR_LOAD = 0x10, CR_SPOUT = 0x40, CRA_TOD50 = 0x80, CRB_ALARM = 0x80 };
enum { CRB_COUNT_A = 0x40, CRB_CNT = 0x20 };

void cia_init(cia_t *c, const uint64_t *now)
{
    memset(c, 0, sizeof(*c));
    c->now = now;
}

static void irq_line(cia_t *c, bool on)
{
    if (on != c->irq) {
        c->irq = on;
        if (c->irq_out) {
            c->irq_out(c, on);
        }
    }
}

/* Flags set at cycle 'at': the IRQ line follows a cycle later */
static void flag(cia_t *c, uint8_t bits, uint64_t at)
{
    c->icr |= bits;
    if (!c->irq && (c->icr & c->imr & 0x1F) && at + 1 < c->irq_at) {
        c->irq_at = at + 1;
    }
}

void cia_reset(cia_t *c)
{
    uint64_t now = *c->now;
    c->pra = c->prb = c->ddra = c->ddrb = 0;
    for (int i = 0; i < 2; i++) {
        cia_timer_t *t = &c->t[i];
        t->counter = t->latch = 0xFFFF;
        t->last = now;
        t->load_at = 0;
        t->zero_at = UINT64_MAX;
        t->counting = false;
        t->stop_at = 0;
        c->cr[i] = 0;
    }
    c->icr = c->imr = 0;
    c->sdr = 0;
    c->sdr_bits = 0;
    memset(c->tod, 0, sizeof(c->tod));
    c->tod[3] = 0x01;
    memset(c->alarm, 0, sizeof(c->alarm));
    c->tod_latched = c->tod_stopped = false;
    c->tod_ticks = 0;
    c->b_event_n = 0;
    c->b_event_checked = false;
    c->irq_at = UINT64_MAX;
    c->next_event = UINT64_MAX;
    irq_line(c, false);
    if (c->port_out) {
        c->port_out(c, 0);
        c->port_out(c, 1);
    }
}

/* Timer B counts timer A's underflows (CNT, unconnected, reads high) */
static inline bool b_counts_a(const cia_t *c)
{
    return (c->cr[1] & CRB_COUNT_A) != 0;
}

/* A control value has the timer count cycles (CNT's edges: none come) */
static inline bool counts_phi2(int i, uint8_t cr)
{
    return (cr & CR_START) && !(cr & (i ? CRB_COUNT_A | CRB_CNT : CRB_CNT));
}

static inline bool counts_cycles(const cia_t *c, int i)
{
    return c->t[i].counting;
}

static void underflow(cia_t *c, int i, uint64_t at);

/* Timer B counting A: an underflow of A at cycle 'at' */
static void count_b(cia_t *c, uint64_t at)
{
    if (c->b_event_n < 4) {
        c->b_events[c->b_event_n++] = at;
    }
}

/* Timer B counts A's underflows up to 'now': a zero check a cycle after
 * each (an underflow then: reload, no count), a count a cycle later
 */
static void run_b_events(cia_t *c, uint64_t now)
{
    cia_timer_t *t = &c->t[1];
    while (c->b_event_n > 0) {
        uint64_t at = c->b_events[0];
        bool done = false;
        if (!c->b_event_checked) {
            if (at + 1 > now) {
                return;
            }
            c->b_event_checked = true;
            if (t->counter == 0 && (c->cr[1] & CR_START)) {
                t->counter = t->latch;
                underflow(c, 1, at + 1);
                done = true;
            }
        }
        if (!done) {
            if (at + 2 > now) {
                return;
            }
            t->counter--;
        }
        c->b_event_checked = false;
        memmove(c->b_events, c->b_events + 1, --c->b_event_n * sizeof(c->b_events[0]));
    }
}

static void underflow(cia_t *c, int i, uint64_t at)
{
    c->t[i].zero_at = at;
    c->t[i].toggle = !c->t[i].toggle;
    if (i == 1) {
        flag(c, ICR_TB, at);
    } else {
        flag(c, ICR_TA, at);
        if ((c->cr[1] & CR_START) && b_counts_a(c)) {
            count_b(c, at);
        }
        if ((c->cr[0] & CR_SPOUT) && c->sdr_bits > 0) {
            /* a bit every two underflows */
            if (--c->sdr_bits == 0) {
                flag(c, ICR_SDR, at);
                if (c->sdr_out) {
                    c->sdr_out(c, c->sdr);
                }
            }
        }
    }
    /* one-shot as of either of the two cycles before */
    const cia_timer_t *t = &c->t[i];
    if ((c->cr[i] & CR_ONESHOT) || (t->oneshot_before && t->control_at + 1 == at)) {
        c->cr[i] &= ~CR_START;
        c->t[i].counting = false;
        c->t[i].stop_at = 0;
    }
}

/* Count timer i's cycles up to 'now' */
static void run_timer(cia_t *c, int i, uint64_t now)
{
    cia_timer_t *t = &c->t[i];
    if (t->load_underflow_at && t->load_underflow_at <= now) {
        uint64_t at = t->load_underflow_at;
        t->load_underflow_at = 0;
        underflow(c, i, at);
    }
    uint64_t limit = t->stop_at && t->stop_at < now ? t->stop_at : now;
    while (counts_cycles(c, i)) {
        uint64_t u = t->last + t->counter;      /* when it reads 0 */
        /* the zero check is a cycle ahead of the count: the last count
         * before a stop leaves 0, no underflow
         */
        if (u > limit || (t->stop_at && u >= t->stop_at)) {
            if (limit > t->last) {
                t->counter -= (uint16_t)(limit - t->last);
                t->last = limit;
            }
            break;
        }
        t->counter = t->latch;
        t->last = u + 1;
        underflow(c, i, u);
        /* More underflows by the limit (a short timer, a long wait): all
         * at once, their flag is set already; one at a time where they
         * matter one by one (one-shot, B counting A, the shift
         * register's last bit)
         */
        uint64_t period = t->latch + 1u;
        if (counts_cycles(c, i) && !(c->cr[i] & CR_ONESHOT) && !(i == 0 && (c->cr[1] & CR_START) && b_counts_a(c)) &&
            t->last + t->latch <= limit && !(t->stop_at && t->last + t->latch >= t->stop_at)) {
            uint64_t end = t->stop_at && t->stop_at - 1 < limit ? t->stop_at - 1 : limit;
            uint64_t n = (end - (t->last + t->latch)) / period + 1;
            bool shifting = i == 0 && (c->cr[0] & CR_SPOUT) && c->sdr_bits > 0;
            if (shifting) {
                n = (uint64_t)c->sdr_bits > n ? n : (uint64_t)c->sdr_bits - 1;
                c->sdr_bits -= (int)n;
            }
            if (n > 0) {
                uint64_t last_u = u + n * period;
                t->toggle ^= n & 1;
                t->zero_at = last_u;
                t->last = last_u + 1;
            }
        }
    }
    if (t->stop_at && now >= t->stop_at) {
        t->counting = false;
        t->stop_at = 0;
    }
    if (now > t->last && !t->counting) {
        t->last = now;
    }
}

/* The next moment the CPU must see: an interrupt, the end of the shift
 * register's byte (another machine gets it then). Underflows that set only
 * a flag wait: reading the registers brings the timers up to date.
 */
static void schedule(cia_t *c)
{
    uint64_t next = c->irq_at, u;
    uint8_t want = c->imr & 0x1F;
    bool shifting = (c->cr[0] & CR_SPOUT) && c->sdr_bits > 0;
    bool b_from_a = (c->cr[1] & CR_START) && b_counts_a(c) && (want & ICR_TB);
    const cia_timer_t *ta = &c->t[0], *tb = &c->t[1];
    if (counts_cycles(c, 0) && ((want & ICR_TA) || shifting || b_from_a)) {
        u = ta->last + ta->counter;
        if (shifting && !(want & ICR_TA) && !b_from_a) {
            u += (uint64_t)(c->sdr_bits - 1) * (ta->latch + 1u);    /* the last bit */
        }
        if (ta->stop_at && ta->stop_at < u) {
            u = ta->stop_at;
        }
        next = u < next ? u : next;
    }
    if (counts_cycles(c, 1) && (want & ICR_TB)) {
        u = tb->last + tb->counter;
        if (tb->stop_at && tb->stop_at < u) {
            u = tb->stop_at;
        }
        next = u < next ? u : next;
    }
    if (c->b_event_n > 0 && (want & ICR_TB)) {
        u = c->b_events[0] + (c->b_event_checked ? 2 : 1);
        next = u < next ? u : next;
    }
    for (int i = 0; i < 2; i++) {
        u = c->t[i].load_underflow_at;
        if (u && u < next && (want & (i ? ICR_TB : ICR_TA))) {
            next = u;
        }
    }
    c->next_event = next;
    if (c->cpu) {
        m6502_lower_until(c->cpu, next);
    }
}

void cia_update(cia_t *c)
{
    cia_update_to(c, *c->now);
}

void cia_update_to(cia_t *c, uint64_t now)
{
    /* A first: B may count its underflows */
    run_timer(c, 0, now);
    run_b_events(c, now);
    run_timer(c, 1, now);
    if (c->irq_at <= now) {
        c->irq_time = c->irq_at;
        c->irq_at = UINT64_MAX;
        irq_line(c, true);
    }
    schedule(c);
}

static uint16_t timer_value(const cia_t *c, int i)
{
    const cia_timer_t *t = &c->t[i];
    uint64_t now = *c->now;
    if (now < t->load_at) {
        /* the load was written at load_at - 2 */
        return t->before - (t->before_counts ? (uint16_t)(now - (t->load_at - 2)) : 0);
    }
    return t->counter;
}

static void timer_control(cia_t *c, int i, uint8_t v)
{
    cia_timer_t *t = &c->t[i];
    uint64_t now = *c->now;
    if ((v & CR_START) && !(c->cr[i] & CR_START)) {
        t->toggle = true;               /* starting sets the toggle output */
    }
    t->oneshot_before = c->cr[i] & CR_ONESHOT;
    t->control_at = now;
    bool was_counting = counts_cycles(c, i);
    uint64_t old_zero = t->last + t->counter;
    bool old_is_zero = t->counter == 0;
    if (v & CR_LOAD) {
        t->before = timer_value(c, i);
        t->before_counts = was_counting && now >= t->last;
    }
    c->cr[i] = v & ~CR_LOAD;
    if (v & CR_LOAD) {
        t->counter = t->latch;
        t->load_at = now + 2;
        t->last = now + 3;
    }
    if (counts_phi2(i, v)) {
        if (!t->counting) {
            t->counting = true;
            if (!(v & CR_LOAD)) {
                t->last = now + 2;
            }
        }
        t->stop_at = 0;
    } else if (t->counting && !t->stop_at) {
        t->stop_at = now + 2;
    }
    /* the old count goes on until a load shows, and may underflow (a
     * stopped one at 0 too, when the load starts it)
     */
    if (v & CR_LOAD) {
        if (was_counting) {
            t->load_underflow_at = old_zero <= now + 2 &&
                                   !(t->stop_at && old_zero >= t->stop_at) ? old_zero : 0;
        } else {
            t->load_underflow_at = t->counting && old_is_zero ? now + 2 : 0;
        }
    }
}

/* ---- Time of day: BCD tenths, seconds, minutes, hours (12 h, bit 7 PM) ---- */

static uint8_t bcd_inc(uint8_t v)
{
    v++;
    if ((v & 0x0F) > 9) {
        v = (v & 0xF0) + 0x10;
    }
    return v;
}

void cia_tod_tick(cia_t *c)
{
    if (c->tod_stopped) {
        return;
    }
    if (++c->tod_ticks < ((c->cr[0] & CRA_TOD50) ? 5 : 6)) {
        return;
    }
    c->tod_ticks = 0;
    uint8_t *t = c->tod;
    t[0] = (t[0] + 1) & 0x0F;
    if (t[0] > 9) {
        t[0] = 0;
        t[1] = bcd_inc(t[1]);
        if (t[1] >= 0x60) {
            t[1] = 0;
            t[2] = bcd_inc(t[2]);
            if (t[2] >= 0x60) {
                t[2] = 0;
                uint8_t pm = t[3] & 0x80, h = t[3] & 0x1F;
                h = bcd_inc(h);
                if (h == 0x12) {
                    pm ^= 0x80;
                } else if (h == 0x13) {
                    h = 0x01;
                }
                t[3] = pm | h;
            }
        }
    }
    if (memcmp(c->tod, c->alarm, 4) == 0) {
        flag(c, ICR_ALARM, *c->now);
        schedule(c);
    }
}

/* ---- Registers ---- */

/* Timer i's output on PB6/PB7: the toggle, or a pulse on the underflow cycle */
static bool timer_out(const cia_t *c, int i)
{
    return (c->cr[i] & CR_TOGGLE) ? c->t[i].toggle : c->t[i].zero_at == *c->now;
}

uint8_t cia_port_value(cia_t *c, int port)
{
    uint8_t out = port ? (c->prb | ~c->ddrb) : (c->pra | ~c->ddra);
    uint8_t ext = c->port_in ? c->port_in(c, port) : 0xFF;
    uint8_t v = out & ext;
    if (port == 1) {
        if (c->cr[0] & CR_PBON) {
            v = (v & ~0x40) | (timer_out(c, 0) << 6);
        }
        if (c->cr[1] & CR_PBON) {
            v = (v & ~0x80) | (timer_out(c, 1) << 7);
        }
    }
    return v;
}

uint8_t cia_read(cia_t *c, uint8_t reg)
{
    reg &= 0x0F;
    /* the ports don't need the timers (unless they put out on PB6/PB7) */
    if (reg >= 4 || (reg == 1 && ((c->cr[0] | c->cr[1]) & CR_PBON))) {
        cia_update(c);
    }
    switch (reg) {
    case 0x0: return cia_port_value(c, 0);
    case 0x1: return cia_port_value(c, 1);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return timer_value(c, 0);
    case 0x5: return timer_value(c, 0) >> 8;
    case 0x6: return timer_value(c, 1);
    case 0x7: return timer_value(c, 1) >> 8;
    case 0x8: {
        uint8_t v = c->tod_latched ? c->latch[0] : c->tod[0];
        c->tod_latched = false;
        return v;
    }
    case 0x9: return c->tod_latched ? c->latch[1] : c->tod[1];
    case 0xA: return c->tod_latched ? c->latch[2] : c->tod[2];
    case 0xB:
        memcpy(c->latch, c->tod, 4);
        c->tod_latched = true;
        return c->latch[3];
    case 0xC: return c->sdr;
    case 0xD: {
        uint8_t v = c->icr | (c->irq ? 0x80 : 0);
        c->icr = 0;
        c->irq_at = UINT64_MAX;
        irq_line(c, false);
        return v;
    }
    case 0xE: return c->cr[0];
    default: return c->cr[1];
    }
}

void cia_write(cia_t *c, uint8_t reg, uint8_t v)
{
    reg &= 0x0F;
    if (reg < 4) {
        /* the ports and their directions: no timers involved */
        switch (reg) {
        case 0x0: c->pra = v; break;
        case 0x1: c->prb = v; break;
        case 0x2: c->ddra = v; break;
        default: c->ddrb = v; break;
        }
        if (c->port_out) {
            c->port_out(c, reg & 1);
        }
        return;
    }
    cia_update(c);
    switch (reg) {
    case 0x0: c->pra = v; if (c->port_out) c->port_out(c, 0); break;
    case 0x1: c->prb = v; if (c->port_out) c->port_out(c, 1); break;
    case 0x2: c->ddra = v; if (c->port_out) c->port_out(c, 0); break;
    case 0x3: c->ddrb = v; if (c->port_out) c->port_out(c, 1); break;
    case 0x4: case 0x6: {
        cia_timer_t *t = &c->t[reg >> 1 & 1];
        t->latch = (t->latch & 0xFF00) | v;
        if (t->zero_at == *c->now) {
            t->counter = t->latch;      /* reloading in this very cycle */
        }
        break;
    }
    case 0x5: case 0x7: {
        /* a stopped timer loads the latch with its high byte */
        int i = reg >> 1 & 1;
        cia_timer_t *t = &c->t[i];
        t->latch = (t->latch & 0x00FF) | (v << 8);
        if (!(c->cr[i] & CR_START) || t->zero_at == *c->now) {
            t->counter = t->latch;
        }
        break;
    }
    case 0x8: case 0x9: case 0xA: case 0xB: {
        int i = reg & 3;
        if (i == 3) {
            v &= 0x9F;
        } else if (i == 0) {
            v &= 0x0F;
        } else {
            v &= 0x7F;
        }
        if (c->cr[1] & CRB_ALARM) {
            c->alarm[i] = v;
        } else {
            c->tod[i] = v;
            if (i == 3) {
                c->tod_stopped = true;  /* until tenths are written */
            } else if (i == 0) {
                c->tod_stopped = false;
                c->tod_ticks = 0;
            }
        }
        break;
    }
    case 0xC:
        c->sdr = v;
        if (c->cr[0] & CR_SPOUT) {
            c->sdr_bits = 16;           /* 8 bits, two timer A underflows each */
        }
        break;
    case 0xD:
        if (v & 0x80) {
            c->imr |= v & 0x1F;
        } else {
            c->imr &= ~(v & 0x1F);
        }
        /* the mask counts from the next cycle; clearing it leaves an IRQ
         * already raised on until ICR is read
         */
        flag(c, 0, *c->now + 1);
        break;
    case 0xE:
        timer_control(c, 0, v);
        break;
    default:
        timer_control(c, 1, v);
        break;
    }
    schedule(c);
}

void cia_sdr_in(cia_t *c, uint8_t byte)
{
    cia_update(c);
    if (!(c->cr[0] & CR_SPOUT)) {
        c->sdr = byte;
        flag(c, ICR_SDR, *c->now);
    }
    schedule(c);
}
