/*
 * Commodore 64 and Commodore 128 (PAL).
 *
 * The 6510/8502 (esp32emu's 6502 core, every bus cycle an access), the
 * VIC-II with its bad lines and sprite DMA stealing cycles, two 6526 CIAs
 * (keyboard, joysticks, timers, TOD, the serial bus), the SID, the PLA's
 * banking and cartridges (.CRT). The C128 adds the MMU, 2 MHz mode, the
 * VDC's 80 columns, the extra keys and the Z80 (esp32emu's core) for CP/M.
 *
 * Floppy drives are emulated whole on the serial bus: their own 6502 runs
 * the drive's DOS ROM with its VIAs and the disk as GCR (1541-II for the
 * C64, 1571 for the C128), so fast loaders work. Images: .D64, .D71, .G64.
 * Programs (.PRG, .T64) can also be put into memory directly.
 *
 * Platform independent: the caller provides the ROMs and the big buffers
 * and drives the emulation one video frame at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CBM_MODEL_C64,
    CBM_MODEL_C128,
    CBM_MODEL_COUNT
} cbm_model_t;

/* The picture: the VIC-II's 320 x 200 with 40 pixels of border left and
 * right and 50 lines above and below (PAL raster lines 1-300); the board
 * doubles it to 800 x 600. On the C128 the VDC's 80 columns are shown
 * instead while 40/80 DISPLAY is down: 640 x 200 drawn 800 x 200 (each
 * character cell 10 pixels wide), tripled by the board (see
 * cbm_screen_mode()).
 */
#define CBM_SCREEN_WIDTH        400
#define CBM_SCREEN_HEIGHT       300
#define CBM_VDC_WIDTH           800
#define CBM_VDC_HEIGHT          200
#define CBM_SCREEN_HZ           50
#define CBM_CLOCK_HZ            985248          /* PAL phi2 */
#define CBM_LINE_CYCLES         63
#define CBM_LINES               312
#define CBM_FRAME_CYCLES        (CBM_LINE_CYCLES * CBM_LINES)   /* 19656 */

#define CBM_DRIVES              2               /* units 8 and 9 */

typedef struct {
    cbm_model_t model;
    uint8_t *ram;               /* 64 KB (C64) or 128 KB (C128) */
    /* ROMs as dumped */
    const uint8_t *basic;       /* C64: 8 KB; C128: 32 KB (BASIC 7.0, $4000-$BFFF) */
    const uint8_t *kernal;      /* C64: 8 KB; C128: 16 KB (editor, Z80 BIOS, KERNAL) */
    const uint8_t *chargen;     /* C64: 4 KB; C128: 8 KB */
    const uint8_t *c64_rom;     /* C128: its C64 mode's BASIC + KERNAL (16 KB) */
    const uint8_t *drive_rom;   /* 1541-II: 16 KB; 1571: 32 KB; NULL: no drives */
    /* CBM_SCREEN_WIDTH x CBM_SCREEN_HEIGHT RGB565 pixels (and on the C128
     * the VDC's CBM_VDC_WIDTH x CBM_VDC_HEIGHT)
     */
    uint16_t *frame;
    uint16_t *vdc_frame;
} cbm_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    uint8_t port01;             /* the 6510's memory configuration */
    bool z80;                   /* C128: the Z80 has the bus */
    bool fast;                  /* C128: 2 MHz */
    bool vdc;                   /* C128: the 80-column screen is shown */
    uint16_t raster;
    bool drive_led[CBM_DRIVES], drive_motor[CBM_DRIVES];
    uint8_t drive_track[CBM_DRIVES];    /* half tracks / 2 */
    uint16_t drive_pc[CBM_DRIVES];
} cbm_status_t;

bool cbm_init(const cbm_config_t *config);
void cbm_reset(void);               /* cold reset, drives too */
void cbm_run_frame(void);
void cbm_get_status(cbm_status_t *status);
cbm_model_t cbm_model(void);
uint8_t cbm_peek(uint16_t address); /* as the CPU sees it, no side effects */

/* ---- Screen ---- */

/* The picture shown now: its size (VIC-II or, on a C128 in 80 columns,
 * the VDC's)
 */
void cbm_screen_mode(int *width, int *height);
void cbm_render_line_rgb565(int y, uint16_t *dst);
uint32_t cbm_screen_signature(void);

/* ---- Sound: 16-bit stereo, the SID in both channels ---- */

#define CBM_AUDIO_HZ            30789           /* 32 cycles per sample */

void cbm_audio_enable(bool enable);
int cbm_audio_render(int16_t *dst, int frames);
int cbm_audio_pending(void);

/* ---- Drives 8 and 9 (0, 1): disk images ----
 *
 * .D64 (35 or 40 tracks, with or without error bytes), .D71 (both sides,
 * on a 1571), .G64 (GCR as it is on the disk, copy protections included).
 * Read into memory as GCR; writes go back to the file with cbm_disk_flush()
 * (D64/D71: sectors decoded from the GCR). Not while cbm_run_frame() runs.
 */
bool cbm_disk_insert(int drive, const char *path);
void cbm_disk_eject(int drive);
const char *cbm_disk_path(int drive);       /* NULL when empty */
bool cbm_disk_write_protected(int drive);
bool cbm_disk_dirty(void);
void cbm_disk_flush(void);

/* A formatted blank disk: CBM_D64_BYTES or CBM_D71_BYTES */
#define CBM_D64_BYTES           174848
#define CBM_D71_BYTES           349696
bool cbm_disk_create(const char *path, uint32_t bytes);

typedef struct {
    int light;                  /* drive with its LED on, -1 none */
    uint32_t reads[CBM_DRIVES]; /* sectors read and written (by the GCR) */
    uint32_t writes[CBM_DRIVES];
    uint8_t track[CBM_DRIVES];
    bool motor[CBM_DRIVES];
} cbm_disk_status_t;

void cbm_disk_status(cbm_disk_status_t *status);

/* ---- Cartridge (.CRT) ----
 *
 * Normal 8 and 16 KB, Ultimax, Ocean, Fun Play, Super Games, System 3,
 * Dinamic, Magic Desk, Simons' BASIC, EasyFlash (read-only). Inserting
 * resets. Not while cbm_run_frame() runs.
 */
bool cbm_cart_insert(const char *path);
void cbm_cart_eject(void);
const char *cbm_cart_path(void);
const char *cbm_cart_info(void);            /* "Ocean, 128 KB" */

/* ---- Programs (.PRG, .T64): into memory as LOAD would put them, then
 * RUN (typed) for BASIC programs. Not while cbm_run_frame() runs.
 */
bool cbm_program_load(const char *path, bool run);

/* ---- Keyboard and joysticks: safe to call from another task ---- */

/* Key number: matrix row * 8 + column as the CIA scans it (row = the bit
 * on port A, column = the bit read on port B): 0 INST/DEL, 1 RETURN,
 * 2 CRSR RIGHT, 3 F7, 4 F1, 5 F3, 6 F5, 7 CRSR DOWN, ... 63 RUN/STOP.
 * The C128's extra keys are 64-87 (rows K0-K2), RESTORE is 88, CAPS LOCK
 * 89, 40/80 DISPLAY 90.
 */
enum {
    CBM_KEY_RESTORE = 88, CBM_KEY_CAPS_LOCK = 89, CBM_KEY_40_80 = 90,
    CBM_KEYS = 91,
};
void cbm_key(uint8_t key, bool down);
void cbm_keys_up(void);

/* Type text: one key every few frames. PETSCII-ish: letters as unshifted
 * keys, '\n' is RETURN. Replaces what's still queued.
 */
void cbm_type(const char *text);

/* The key for an ASCII character (*shift: with SHIFT); -1 if none */
int cbm_ascii_key(char c, bool *shift);

enum {
    CBM_JOY_UP = 0x01, CBM_JOY_DOWN = 0x02, CBM_JOY_LEFT = 0x04, CBM_JOY_RIGHT = 0x08,
    CBM_JOY_FIRE = 0x10,
};
/* Port 1 or 2 */
void cbm_joystick(int port, uint8_t bits);
