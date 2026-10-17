/*
 * Keyboard and joysticks, from other tasks (Bluetooth, console).
 *
 * The C64's keyboard is an 8 x 8 matrix: CIA 1 port A selects rows (low),
 * port B reads the columns (low = pressed); scanning the other way round
 * works too. The C128 adds rows K0-K2 (selected through VIC $D02F) and
 * three keys outside the matrix (RESTORE, CAPS LOCK, 40/80 DISPLAY).
 *
 *        PB0   PB1    PB2    PB3   PB4    PB5  PB6   PB7
 *   PA0  DEL   RETURN CRSR-> F7    F1     F3   F5    CRSR-DOWN
 *   PA1  3     W      A      4     Z      S    E     LSHIFT
 *   PA2  5     R      D      6     C      F    T     X
 *   PA3  7     Y      G      8     B      H    U     V
 *   PA4  9     I      J      0     M      K    O     N
 *   PA5  +     P      L      -     .      :    @     ,
 *   PA6  POUND *      ;      HOME  RSHIFT =    ^     /
 *   PA7  1     <-     CTRL   2     SPACE  C=   Q     RUN/STOP
 */
#include <string.h>
#include "cbm_internal.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()      portENTER_CRITICAL(&s_lock)
#define UNLOCK()    portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QUEUE_SIZE  64

enum { EV_KEY, EV_JOY, EV_KEYS_UP };

typedef struct {
    uint8_t type, code, value;
} input_event_t;

static input_event_t s_queue[QUEUE_SIZE];
static volatile uint32_t s_head, s_tail;

static struct {
    uint8_t row[11];            /* pressed columns per row (bit set = pressed); 8-10: K0-K2 */
    bool restore, caps, d40_80;
    uint8_t joy[2];             /* active-high CBM_JOY_* */
} in;

static void push(const input_event_t *ev)
{
    LOCK();
    if (s_head - s_tail < QUEUE_SIZE) {
        s_queue[s_head % QUEUE_SIZE] = *ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    UNLOCK();
}

void cbm_key(uint8_t key, bool down)
{
    input_event_t ev = { .type = EV_KEY, .code = key, .value = down };
    push(&ev);
}

void cbm_joystick(int port, uint8_t bits)
{
    input_event_t ev = { .type = EV_JOY, .code = port, .value = bits };
    push(&ev);
}

void cbm_keys_up(void)
{
    input_event_t ev = { .type = EV_KEYS_UP };
    push(&ev);
}

static void set_key(int key, bool down)
{
    if (key < 0) {
        return;
    }
    if (key < 88) {
        uint8_t bit = 1u << (key & 7);
        in.row[key >> 3] = down ? in.row[key >> 3] | bit : in.row[key >> 3] & ~bit;
    } else if (key == CBM_KEY_RESTORE) {
        in.restore = down;
    } else if (key == CBM_KEY_CAPS_LOCK) {
        if (down) {
            in.caps = !in.caps;         /* a locking key */
        }
    } else if (key == CBM_KEY_40_80) {
        if (down) {
            in.d40_80 = !in.d40_80;
        }
    }
}

/* ---- Typing ---- */

#define TYPE_MAX    256
#define KEY_LSHIFT  15

static char s_type[TYPE_MAX];
static volatile int s_type_len;
static int s_type_pos, s_type_frame;

int cbm_ascii_key(char c, bool *shift)
{
    static const char plain[64] = {
        0, '\n', 0, 0, 0, 0, 0, 0,          '3', 'w', 'a', '4', 'z', 's', 'e', 0,
        '5', 'r', 'd', '6', 'c', 'f', 't', 'x', '7', 'y', 'g', '8', 'b', 'h', 'u', 'v',
        '9', 'i', 'j', '0', 'm', 'k', 'o', 'n', '+', 'p', 'l', '-', '.', ':', '@', ',',
        0, '*', ';', 0, 0, '=', '^', '/',   '1', 0, 0, '2', ' ', 0, 'q', 0,
    };
    static const char shifted[64] = {
        0, 0, 0, 0, 0, 0, 0, 0,             '#', 0, 0, '$', 0, 0, 0, 0,
        '%', 0, 0, '&', 0, 0, 0, 0,         '\'', 0, 0, '(', 0, 0, 0, 0,
        ')', 0, 0, 0, 0, 0, 0, 0,           0, 0, 0, 0, '>', '[', 0, '<',
        0, 0, ']', 0, 0, 0, 0, '?',         '!', 0, 0, '"', 0, 0, 0, 0,
    };
    char lower = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    *shift = lower != c;
    for (int k = 0; k < 64; k++) {
        if (plain[k] && plain[k] == lower) {
            return k;
        }
    }
    for (int k = 0; k < 64; k++) {
        if (shifted[k] && shifted[k] == c) {
            *shift = true;
            return k;
        }
    }
    return -1;
}

void cbm_type(const char *text)
{
    LOCK();
    size_t n = strlen(text);
    if (n >= TYPE_MAX) {
        n = TYPE_MAX - 1;
    }
    memcpy(s_type, text, n);
    s_type[n] = 0;
    s_type_len = n;
    s_type_pos = 0;
    s_type_frame = 0;
    UNLOCK();
}

/* Each character: down for 2 frames, up for 2 (the KERNAL scans 50 times
 * a second and wants a key let go before the same one counts again); 8
 * and 8 under CP/M
 */
static void type_frame(void)
{
    if (s_type_pos >= s_type_len) {
        return;
    }
    bool shift;
    int key = cbm_ascii_key(s_type[s_type_pos], &shift);
    int phase = s_type_frame++;
    if (key < 0) {
        s_type_pos++;
        s_type_frame = 0;
        return;
    }
    /* CP/M on the C128 scans one row an interrupt: longer there (chosen
     * once a key: the CPUs take turns)
     */
    static int hold;
    if (phase == 0) {
        hold = c128_z80_active() ? 8 : 2;
    }
    if (phase == 0) {
        if (shift) {
            set_key(KEY_LSHIFT, true);
        }
        set_key(key, true);
    } else if (phase == hold) {
        set_key(key, false);
        set_key(KEY_LSHIFT, false);
    } else if (phase >= hold * 2) {
        s_type_pos++;
        s_type_frame = 0;
    }
}

void input_frame(void)
{
    type_frame();
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    while (s_tail != head) {
        input_event_t ev = s_queue[s_tail % QUEUE_SIZE];
        __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
        switch (ev.type) {
        case EV_KEY:
            set_key(ev.code, ev.value);
            break;
        case EV_JOY:
            if (ev.code >= 1 && ev.code <= 2) {
                in.joy[ev.code - 1] = ev.value;
            }
            break;
        default:
            memset(in.row, 0, sizeof(in.row));
            in.restore = false;
            in.joy[0] = in.joy[1] = 0;
            break;
        }
    }
}

/* ---- As the CIA sees it ---- */

uint8_t input_columns(uint8_t rows)
{
    uint8_t pressed = 0;
    for (int r = 0; r < 8; r++) {
        if (!(rows & (1u << r))) {
            pressed |= in.row[r];
        }
    }
    return ~pressed;
}

uint8_t input_k_columns(uint8_t k_rows)
{
    uint8_t pressed = 0;
    for (int r = 0; r < 3; r++) {
        if (!(k_rows & (1u << r))) {
            pressed |= in.row[8 + r];
        }
    }
    return ~pressed;
}

uint8_t input_rows(uint8_t columns)
{
    uint8_t pulled = 0;
    for (int r = 0; r < 8; r++) {
        if (in.row[r] & ~columns) {
            pulled |= 1u << r;
        }
    }
    return ~pulled;
}

uint8_t input_joystick(int port)
{
    return ~(in.joy[(port - 1) & 1] & 0x1F);
}

bool input_restore(void)
{
    return in.restore;
}

bool input_caps_lock(void)
{
    return in.caps;
}

bool input_40_80(void)
{
    return in.d40_80;
}

void input_reset(void)
{
    memset(in.row, 0, sizeof(in.row));
    in.restore = false;
}
