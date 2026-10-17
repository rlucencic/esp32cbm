/*
 * MOS 6522 VIA, as the drives use it: two ports with their control lines
 * (CA1 and CB1 edges, CA2 and CB2 as outputs), timer 1 (one-shot or free
 * running), timer 2 (one-shot), the interrupt flags and enable mask.
 *
 * Timers run lazily, like the CIA's: via_update() counts them up to the
 * drive CPU's cycle counter; next_event is the next timer 1 or 2 underflow.
 */
#include <string.h>
#include "via.h"
#include "m6502.h"

enum { IFR_CA2 = 0x01, IFR_CA1 = 0x02, IFR_SR = 0x04, IFR_CB2 = 0x08, IFR_CB1 = 0x10, IFR_T2 = 0x20, IFR_T1 = 0x40 };

void via_init(via_t *v, const uint64_t *now)
{
    memset(v, 0, sizeof(*v));
    v->now = now;
}

static void irq(via_t *v)
{
    bool on = (v->ifr & v->ier & 0x7F) != 0;
    if (on != v->irq) {
        v->irq = on;
        if (v->irq_out) {
            v->irq_out(v, on);
        }
    }
}

void via_reset(via_t *v)
{
    v->ora = v->orb = v->ddra = v->ddrb = 0;
    v->acr = v->pcr = 0;
    v->ifr = v->ier = 0;
    v->t1 = v->t1_latch = 0xFFFF;
    v->t2 = 0xFFFF;
    v->t1_armed = v->t2_armed = false;
    v->last = *v->now;
    v->next_event = UINT64_MAX;
    irq(v);
    if (v->port_out) {
        v->port_out(v, 0);
        v->port_out(v, 1);
    }
}

static void schedule(via_t *v)
{
    uint64_t next = UINT64_MAX;
    if (v->t1_armed || (v->acr & 0x40)) {
        next = v->last + v->t1 + 1;
    }
    if (v->t2_armed && !(v->acr & 0x20)) {
        uint64_t t2 = v->last + v->t2 + 1;
        next = t2 < next ? t2 : next;
    }
    v->next_event = next;
    if (v->cpu) {
        m6502_lower_until(v->cpu, next);
    }
}

/* The next timer underflow that interrupts (UINT64_MAX: none; now: one
 * is waiting)
 */
uint64_t via_next_irq(const via_t *v)
{
    if (v->irq) {
        return *v->now;
    }
    uint64_t next = UINT64_MAX;
    if ((v->ier & IFR_T1) && (v->t1_armed || (v->acr & 0x40))) {
        next = v->last + v->t1 + 1;
    }
    if ((v->ier & IFR_T2) && v->t2_armed && !(v->acr & 0x20)) {
        uint64_t t2 = v->last + v->t2 + 1;
        next = t2 < next ? t2 : next;
    }
    return next;
}

void via_update(via_t *v)
{
    uint64_t now = *v->now;
    if (now <= v->last) {
        return;
    }
    uint32_t n = (uint32_t)(now - v->last);
    v->last = now;
    /* timer 1: underflow after t1 + 1 cycles; free running reloads (N + 2) */
    uint32_t left = n;
    while (left) {
        if (v->t1 >= left) {
            v->t1 -= left;
            break;
        }
        left -= v->t1 + 1;
        if (v->t1_armed || (v->acr & 0x40)) {
            v->ifr |= IFR_T1;
            if (v->acr & 0x80) {
                v->pb7 = !v->pb7;
            }
        }
        if (v->acr & 0x40) {
            v->t1 = v->t1_latch + 1;    /* the extra cycle of the reload */
        } else {
            v->t1_armed = false;
            v->t1 = 0xFFFF;
        }
    }
    /* timer 2 (one-shot; counts on after its interrupt) */
    if (!(v->acr & 0x20)) {
        if (v->t2_armed && v->t2 < n) {
            v->ifr |= IFR_T2;
            v->t2_armed = false;
        }
        v->t2 -= (uint16_t)n;
    }
    irq(v);
    schedule(v);
}

/* ---- Ports ---- */

static uint8_t port_value(via_t *v, int port)
{
    uint8_t out = port ? v->orb : v->ora, ddr = port ? v->ddrb : v->ddra;
    uint8_t in = v->port_in ? v->port_in(v, port) : 0xFF;
    uint8_t value = (out & ddr) | (in & ~ddr);
    if (port && (v->acr & 0x80)) {
        value = (value & 0x7F) | (v->pb7 ? 0x80 : 0);
    }
    return value;
}

void via_ca1(via_t *v, bool level)
{
    if (level != v->ca1) {
        bool rising = level;
        v->ca1 = level;
        if (rising == ((v->pcr & 0x01) != 0)) {
            via_update(v);
            v->ifr |= IFR_CA1;
            irq(v);
        }
    }
}

void via_cb1(via_t *v, bool level)
{
    if (level != v->cb1) {
        bool rising = level;
        v->cb1 = level;
        if (rising == ((v->pcr & 0x10) != 0)) {
            via_update(v);
            v->ifr |= IFR_CB1;
            irq(v);
        }
    }
}

uint8_t via_read(via_t *v, uint8_t reg)
{
    via_update(v);
    switch (reg & 0x0F) {
    case 0x0:
        v->ifr &= ~(IFR_CB1 | IFR_CB2);
        irq(v);
        return port_value(v, 1);
    case 0x1:
    case 0xF:
        if ((reg & 0x0F) == 1) {
            v->ifr &= ~(IFR_CA1 | IFR_CA2);
            irq(v);
        }
        if ((v->acr & 0x01) && v->port_a_latched) {
            return (v->ora & v->ddra) | (v->ira_latch & ~v->ddra);
        }
        return port_value(v, 0);
    case 0x2: return v->ddrb;
    case 0x3: return v->ddra;
    case 0x4:
        v->ifr &= ~IFR_T1;
        irq(v);
        return v->t1;
    case 0x5: return v->t1 >> 8;
    case 0x6: return v->t1_latch;
    case 0x7: return v->t1_latch >> 8;
    case 0x8:
        v->ifr &= ~IFR_T2;
        irq(v);
        return v->t2;
    case 0x9: return v->t2 >> 8;
    case 0xA: return v->sr;
    case 0xB: return v->acr;
    case 0xC: return v->pcr;
    case 0xD: return v->ifr | ((v->ifr & v->ier & 0x7F) ? 0x80 : 0);
    default: return v->ier | 0x80;
    }
}

void via_write(via_t *v, uint8_t reg, uint8_t val)
{
    via_update(v);
    switch (reg & 0x0F) {
    case 0x0:
        v->orb = val;
        v->ifr &= ~(IFR_CB1 | IFR_CB2);
        irq(v);
        if (v->port_out) {
            v->port_out(v, 1);
        }
        break;
    case 0x1:
    case 0xF:
        v->ora = val;
        if ((reg & 0x0F) == 1) {
            v->ifr &= ~(IFR_CA1 | IFR_CA2);
            irq(v);
        }
        if (v->port_out) {
            v->port_out(v, 0);
        }
        break;
    case 0x2: v->ddrb = val; if (v->port_out) v->port_out(v, 1); break;
    case 0x3: v->ddra = val; if (v->port_out) v->port_out(v, 0); break;
    case 0x4: case 0x6: v->t1_latch = (v->t1_latch & 0xFF00) | val; break;
    case 0x5:
        v->t1_latch = (v->t1_latch & 0x00FF) | (val << 8);
        v->t1 = v->t1_latch;
        v->ifr &= ~IFR_T1;
        v->t1_armed = true;
        if (v->acr & 0x80) {
            v->pb7 = false;
        }
        irq(v);
        break;
    case 0x7:
        v->t1_latch = (v->t1_latch & 0x00FF) | (val << 8);
        v->ifr &= ~IFR_T1;
        irq(v);
        break;
    case 0x8: v->t2_latch_lo = val; break;
    case 0x9:
        v->t2 = v->t2_latch_lo | (val << 8);
        v->ifr &= ~IFR_T2;
        v->t2_armed = true;
        irq(v);
        break;
    case 0xA: v->sr = val; break;
    case 0xB: v->acr = val; break;
    case 0xC:
        v->pcr = val;
        if (v->port_out) {
            v->port_out(v, 0);          /* CA2/CB2 follow the PCR */
            v->port_out(v, 1);
        }
        break;
    case 0xD:
        v->ifr &= ~(val & 0x7F);
        irq(v);
        break;
    default:
        if (val & 0x80) {
            v->ier |= val & 0x7F;
        } else {
            v->ier &= ~(val & 0x7F);
        }
        irq(v);
        break;
    }
    schedule(v);
}

void via_latch_a(via_t *v, uint8_t value)
{
    v->ira_latch = value;
    v->port_a_latched = true;
}
