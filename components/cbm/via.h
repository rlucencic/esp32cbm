/*
 * MOS 6522 VIA (the drives'): see via.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct via {
    uint8_t ora, orb, ddra, ddrb;
    uint8_t acr, pcr, ifr, ier, sr;
    uint16_t t1, t1_latch, t2;
    uint8_t t2_latch_lo;
    bool t1_armed, t2_armed, pb7;
    bool ca1, cb1;
    uint8_t ira_latch;
    bool port_a_latched;
    bool irq;
    uint64_t last, next_event;
    const uint64_t *now;
    struct m6502 *cpu;                  /* its CPU: stops at next_event */
    uint8_t (*port_in)(struct via *v, int port);
    void (*port_out)(struct via *v, int port);
    void (*irq_out)(struct via *v, bool on);
    void *ctx;
} via_t;

void via_init(via_t *v, const uint64_t *now);
void via_reset(via_t *v);
uint8_t via_read(via_t *v, uint8_t reg);
void via_write(via_t *v, uint8_t reg, uint8_t val);
void via_update(via_t *v);
uint64_t via_next_irq(const via_t *v);
/* Outputs, inputs pulled high */
static inline uint8_t via_port_out(const via_t *v, int port)
{
    return port ? (v->orb | ~v->ddrb) : (v->ora | ~v->ddra);
}

/* CA2/CB2 as outputs: PCR modes 110 (low) and 111 (high); handshake and
 * pulse modes count as high
 */
static inline bool via_ca2(const via_t *v)
{
    return (v->pcr & 0x0E) != 0x0C;
}

static inline bool via_cb2(const via_t *v)
{
    return (v->pcr & 0xE0) != 0xC0;
}
void via_ca1(via_t *v, bool level);
void via_cb1(via_t *v, bool level);
void via_latch_a(via_t *v, uint8_t value);          /* a byte on port A, latched */
