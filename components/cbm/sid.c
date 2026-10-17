/*
 * SID (6581/8580) and the sound output.
 *
 * Like the AY on the CPC and the Spectrum: register writes become events
 * stamped with their cycle; the render side (the speaker task) replays them
 * into its own copy of the chip, one output sample per 32 cycles. Voice 3
 * also runs on the emulation side, as far as reading its oscillator
 * ($D41B) and envelope ($D41C) needs (games take random numbers from its
 * noise).
 *
 * Per voice: a 24-bit phase accumulator, triangle / sawtooth / pulse /
 * noise (combinations ANDed), ring modulation and hard sync, the ADSR with
 * the chip's rate periods and exponential decay; then the filter (a state
 * variable one, low / band / high pass with resonance) and the volume,
 * whose DC step makes the $D418 sample trick audible.
 */
#include <math.h>
#include <string.h>
#include "cbm_internal.h"

#define CYCLES_PER_SAMPLE   32
#define EVENT_QUEUE         8192            /* power of two */

/* ---- One voice: shared by the emulation side (voice 3) and the render side ---- */

typedef struct {
    uint32_t acc;                           /* 24 bits */
    uint32_t lfsr;                          /* 23-bit noise */
    uint16_t freq, pw;
    uint8_t ctrl, ad, sr;
    /* envelope */
    uint8_t env;                            /* 0-255 */
    uint8_t state;                          /* ATTACK, DECAY_SUSTAIN, RELEASE */
    uint32_t rate_count, exp_count;
    bool msb_rose;                          /* for sync */
} voice_t;

enum { ATTACK, DECAY_SUSTAIN, RELEASE };

static const uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251,
};

static void voice_reset(voice_t *v)
{
    memset(v, 0, sizeof(*v));
    v->lfsr = 0x7FFFF8;
    v->state = RELEASE;
}

static void voice_ctrl(voice_t *v, uint8_t ctrl)
{
    bool gate = ctrl & 1, was = v->ctrl & 1;
    if (gate && !was) {
        v->state = ATTACK;
    } else if (!gate && was) {
        v->state = RELEASE;
    }
    if (ctrl & 0x08) {
        v->acc = 0;                         /* test bit */
        v->lfsr = 0x7FFFF8;
    }
    v->ctrl = ctrl;
}

static inline uint8_t exp_period(uint8_t env)
{
    return env >= 93 ? 1 : env >= 54 ? 2 : env >= 26 ? 4 : env >= 14 ? 8 : env >= 6 ? 16 : 30;
}

/* The envelope over `cycles` */
static void env_clock(voice_t *v, uint32_t cycles)
{
    uint8_t rate = v->state == ATTACK ? v->ad >> 4 : v->state == DECAY_SUSTAIN ? v->ad & 15 : v->sr & 15;
    uint32_t period = rate_period[rate];
    v->rate_count += cycles;
    while (v->rate_count >= period) {
        v->rate_count -= period;
        if (v->state == ATTACK) {
            if (++v->env == 0xFF) {
                v->state = DECAY_SUSTAIN;
            }
            continue;
        }
        if (++v->exp_count < exp_period(v->env)) {
            continue;
        }
        v->exp_count = 0;
        if (v->state == DECAY_SUSTAIN) {
            uint8_t sustain = (v->sr >> 4) * 0x11;
            if (v->env > sustain) {
                v->env--;
            }
        } else if (v->env) {
            v->env--;
        }
    }
}

/* The oscillator over `cycles`; the noise LFSR clocks on bit 19's rises */
static void osc_clock(voice_t *v, uint32_t cycles)
{
    if (v->ctrl & 0x08) {
        return;
    }
    uint32_t old = v->acc;
    uint32_t add = (uint32_t)v->freq * cycles;
    v->acc = (old + add) & 0xFFFFFF;
    v->msb_rose = !(old & 0x800000) && (v->acc & 0x800000) ? true : add >= 0x800000;
    /* noise: one clock per rise of bit 19 */
    uint32_t rises = ((old + add) >> 20) - (old >> 20);
    if (!(old & 0x80000) && ((old + add) & 0x80000) && rises == 0) {
        rises = 1;
    }
    if (rises > 24) {
        rises = 24;
    }
    while (rises--) {
        uint32_t bit = ((v->lfsr >> 22) ^ (v->lfsr >> 17)) & 1;
        v->lfsr = ((v->lfsr << 1) | bit) & 0x7FFFFF;
    }
}

static inline uint16_t noise_out(const voice_t *v)
{
    uint32_t l = v->lfsr;
    return ((l >> 20) & 1) << 11 | ((l >> 18) & 1) << 10 | ((l >> 14) & 1) << 9 | ((l >> 11) & 1) << 8 |
           ((l >> 9) & 1) << 7 | ((l >> 5) & 1) << 6 | ((l >> 2) & 1) << 5 | (l & 1) << 4;
}

/* 12-bit waveform output; ring is the modulating voice's accumulator */
static uint16_t wave_out(const voice_t *v, uint32_t ring_acc)
{
    uint8_t w = v->ctrl >> 4;
    if (!w) {
        return 0;
    }
    uint16_t out = 0xFFF;
    if (w & 1) {                            /* triangle */
        uint32_t msb = v->acc & 0x800000;
        if (v->ctrl & 0x04) {
            msb ^= ring_acc & 0x800000;
        }
        out &= ((msb ? ~v->acc : v->acc) >> 11) & 0xFFF;
    }
    if (w & 2) {                            /* sawtooth */
        out &= v->acc >> 12;
    }
    if (w & 4) {                            /* pulse */
        out &= ((v->acc >> 12) >= v->pw) ? 0xFFF : 0;
    }
    if (w & 8) {                            /* noise */
        out &= noise_out(v);
    }
    return out;
}

/* ---- Emulation side ---- */

typedef struct {
    uint64_t time;                          /* cycle */
    uint8_t reg, value;
} sid_event_t;

static struct {
    uint8_t reg[32];
    voice_t v3;                             /* voice 3 for $D41B/$D41C */
    uint64_t v3_time;
    bool enabled;
    sid_event_t *queue;
    volatile uint32_t head, tail;
    volatile uint64_t now;
    uint32_t dropped;
} sid;

static void push_event(uint8_t reg, uint8_t value)
{
    uint64_t t = cbm_now();
    if (t > sid.now) {
        __atomic_store_n(&sid.now, t, __ATOMIC_RELEASE);
    }
    if (!sid.enabled || sid.queue == NULL) {
        return;
    }
    uint32_t head = sid.head;
    if (head - __atomic_load_n(&sid.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        sid.dropped++;
        return;
    }
    sid_event_t *e = &sid.queue[head % EVENT_QUEUE];
    e->time = t;
    e->reg = reg;
    e->value = value;
    __atomic_store_n(&sid.head, head + 1, __ATOMIC_RELEASE);
}

static void v3_update(void)
{
    uint64_t now = cbm_now();
    if (now > sid.v3_time) {
        uint32_t n = (uint32_t)(now - sid.v3_time > 100000 ? 100000 : now - sid.v3_time);
        osc_clock(&sid.v3, n);
        env_clock(&sid.v3, n);
        sid.v3_time = now;
    }
}

static void voice_write(voice_t *v, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case 0: v->freq = (v->freq & 0xFF00) | val; break;
    case 1: v->freq = (v->freq & 0x00FF) | (val << 8); break;
    case 2: v->pw = (v->pw & 0xF00) | val; break;
    case 3: v->pw = (v->pw & 0x0FF) | ((val & 0x0F) << 8); break;
    case 4: voice_ctrl(v, val); break;
    case 5: v->ad = val; break;
    case 6: v->sr = val; break;
    default: break;
    }
}

void sid_write(uint8_t reg, uint8_t v)
{
    reg &= 0x1F;
    if (reg > 0x18) {
        return;
    }
    if (reg >= 14 && reg <= 20) {
        v3_update();
        voice_write(&sid.v3, reg - 14, v);
    }
    sid.reg[reg] = v;
    push_event(reg, v);
}

uint8_t sid_read(uint8_t reg)
{
    switch (reg & 0x1F) {
    case 0x19: case 0x1A: return 0xFF;      /* paddles */
    case 0x1B: v3_update(); return wave_out(&sid.v3, 0) >> 4;
    case 0x1C: v3_update(); return sid.v3.env;
    default: return 0;                      /* write-only */
    }
}

void sid_reset(void)
{
    for (int r = 0; r < 25; r++) {
        sid_write(r, 0);
    }
    voice_reset(&sid.v3);
    sid.v3_time = cbm_now();
}

void sid_frame_end(void)
{
    uint64_t t = cbm_now();
    if (t > sid.now) {
        __atomic_store_n(&sid.now, t, __ATOMIC_RELEASE);
    }
}

/* ---- Render side ---- */

static struct {
    voice_t v[3];
    uint8_t reg[32];
    float lp, bp;                           /* filter state */
    float f, q;                             /* coefficients */
    int32_t dc;
    uint64_t pos;                           /* cycle of the next sample */
} ren;

static void filter_coefficients(void)
{
    uint16_t fc = (ren.reg[0x15] & 7) | (ren.reg[0x16] << 3);
    float hz = 30.0f + fc * 5.8f;           /* 8580-like, 30 Hz - 12 kHz */
    ren.f = 2.0f * sinf(3.14159265f * hz / CBM_AUDIO_HZ);
    if (ren.f > 1.2f) {
        ren.f = 1.2f;
    }
    ren.q = 1.0f / (0.707f + (ren.reg[0x17] >> 4) / 15.0f * 2.0f);
}

static void synth_write(uint8_t reg, uint8_t v)
{
    ren.reg[reg] = v;
    if (reg < 21) {
        voice_write(&ren.v[reg / 7], reg % 7, v);
    } else if (reg <= 0x17) {
        filter_coefficients();
    }
}

static inline int16_t clip16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static int16_t sample(void)
{
    voice_t *v = ren.v;
    for (int i = 0; i < 3; i++) {
        osc_clock(&v[i], CYCLES_PER_SAMPLE);
        env_clock(&v[i], CYCLES_PER_SAMPLE);
    }
    /* hard sync: voice 1 from 3, 2 from 1, 3 from 2 */
    for (int i = 0; i < 3; i++) {
        const voice_t *src = &v[(i + 2) % 3];
        if ((v[i].ctrl & 0x02) && src->msb_rose) {
            v[i].acc = 0;
        }
    }
    float filtered = 0, direct = 0;
    uint8_t route = ren.reg[0x17], mode = ren.reg[0x18];
    for (int i = 0; i < 3; i++) {
        int32_t w = (int32_t)wave_out(&v[i], v[(i + 2) % 3].acc) - 0x800;
        float out = (float)(w * v[i].env) / 256.0f;     /* +-2048 */
        if (route & (1u << i)) {
            filtered += out;
        } else if (!(i == 2 && (mode & 0x80))) {
            direct += out;
        }
    }
    /* state variable filter */
    float hp = filtered - ren.lp - ren.q * ren.bp;
    ren.bp += ren.f * hp;
    ren.lp += ren.f * ren.bp;
    float out = direct;
    if (mode & 0x10) {
        out += ren.lp;
    }
    if (mode & 0x20) {
        out += ren.bp;
    }
    if (mode & 0x40) {
        out += hp;
    }
    /* the volume, with the DC the $D418 trick plays on: three voices at
     * full volume (+-6144, and the DC) just fit 16 bits
     */
    int32_t mix = (int32_t)((out + 1024.0f) * (mode & 15) * 0.3f);
    ren.dc += ((mix << 8) - ren.dc) >> 11;
    return clip16(mix - (ren.dc >> 8));
}

int CBM_HOT cbm_audio_render(int16_t *dst, int frames)
{
    uint64_t limit = __atomic_load_n(&sid.now, __ATOMIC_ACQUIRE);
    uint32_t tail = sid.tail;
    int done = 0;
    while (done < frames && ren.pos + CYCLES_PER_SAMPLE <= limit) {
        uint64_t end = ren.pos + CYCLES_PER_SAMPLE;
        uint32_t head = __atomic_load_n(&sid.head, __ATOMIC_ACQUIRE);
        while (tail != head && sid.queue[tail % EVENT_QUEUE].time < end) {
            synth_write(sid.queue[tail % EVENT_QUEUE].reg, sid.queue[tail % EVENT_QUEUE].value);
            tail++;
        }
        int16_t s = sample();
        *dst++ = s;
        *dst++ = s;
        ren.pos = end;
        done++;
    }
    __atomic_store_n(&sid.tail, tail, __ATOMIC_RELEASE);
    return done;
}

int cbm_audio_pending(void)
{
    uint64_t now = __atomic_load_n(&sid.now, __ATOMIC_ACQUIRE);
    return now > ren.pos ? (int)((now - ren.pos) / CYCLES_PER_SAMPLE) : 0;
}

void cbm_audio_enable(bool enable)
{
    sid.enabled = enable;
}

void sid_init(void)
{
    if (sid.enabled && sid.queue == NULL) {
        sid.queue = cbm_malloc(EVENT_QUEUE * sizeof(sid_event_t));
    }
    sid.head = sid.tail = 0;
    uint64_t now = cbm_now();
    sid.now = now;
    memset(&ren, 0, sizeof(ren));
    for (int i = 0; i < 3; i++) {
        voice_reset(&ren.v[i]);
    }
    filter_coefficients();
    ren.pos = now;
}
