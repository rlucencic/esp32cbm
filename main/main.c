/*
 * Commodore 64 and 128 emulator on the Makerfabs MaTouch ESP32-S3 7".
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Commodore into it: memory, ROMs, settings, keyboard map, joystick,
 * drives 8 and 9, the cartridge and program slots, status.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "cbm.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static const char *const model_names[] = { "C64", "C128", "" };
static const char *const model_full[] = { "Commodore 64", "Commodore 128" };

static cbm_config_t s_cfg;
static uint8_t *s_rom[ROM_COUNT];
static char s_rom_text[128];
static bool s_started;
static char s_program[DISKS_NAME_LEN + 64];     /* path of the program "in the slot" */
static int s_program_wait;                      /* seconds until it goes into memory (0: done) */

/* ---- Memory ----
 *
 * Everything at power-on, before the screen, WiFi and Bluetooth fragment
 * PSRAM: 128 KB RAM, every ROM, the pictures.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < ROM_COUNT; i++) {
        s_rom[i] = heap_caps_malloc(rom_store_size(i), MALLOC_CAP_SPIRAM);
    }
    s_cfg.frame = heap_caps_calloc(CBM_SCREEN_WIDTH * CBM_SCREEN_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_cfg.vdc_frame = heap_caps_calloc(CBM_VDC_WIDTH * CBM_VDC_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disks, cartridges and programs on the SD card" : "No SD card: no disks");
}

/* ---- Starting ---- */

static bool missing(const char *what)
{
    static char message[200];
    snprintf(message, sizeof(message),
             "No %s ROM: put %s into roms/ and flash with idf.py flash.", model_full[settings.model], what);
    emu_set_message(message);
    return false;
}

static bool load(rom_id_t id)
{
    return s_rom[id] && rom_store_load(id, s_rom[id]);
}

static bool start(void)
{
    if (s_cfg.ram == NULL || s_cfg.frame == NULL) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    cbm_model_t model = settings.model;
    s_cfg.model = model;
    static const rom_id_t c64[] = { ROM_C64_BASIC, ROM_C64_KERNAL, ROM_C64_CHARGEN };
    static const rom_id_t c128[] = { ROM_C128_BASIC, ROM_C128_KERNAL, ROM_C128_CHARGEN, ROM_C128_C64 };
    const rom_id_t *need = model == CBM_MODEL_C128 ? c128 : c64;
    int n = model == CBM_MODEL_C128 ? 4 : 3;
    for (int i = 0; i < n; i++) {
        if (!load(need[i])) {
            return missing(rom_store_name(need[i]));
        }
    }
    s_cfg.basic = s_rom[need[0]];
    s_cfg.kernal = s_rom[need[1]];
    s_cfg.chargen = s_rom[need[2]];
    s_cfg.c64_rom = model == CBM_MODEL_C128 ? s_rom[ROM_C128_C64] : NULL;
    rom_id_t drive = model == CBM_MODEL_C128 ? ROM_1571 : ROM_1541;
    s_cfg.drive_rom = load(drive) ? s_rom[drive] : NULL;
    snprintf(s_rom_text, sizeof(s_rom_text), "%s, %s, %s%s%s", rom_store_name(need[0]), rom_store_name(need[1]),
             rom_store_name(need[2]), s_cfg.drive_rom ? ", " : " (no drive ROM: no disks)",
             s_cfg.drive_rom ? rom_store_name(drive) : "");
    if (!cbm_init(&s_cfg)) {
        emu_set_message("The Commodore could not be set up: see the log.");
        return false;
    }
    s_started = true;
    /* a program in the slot goes in once BASIC is up */
    s_program_wait = s_program[0] ? 4 : 0;
    ESP_LOGI(TAG, "%s: %s", model_full[model], s_rom_text);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    int w, h;
    cbm_screen_mode(&w, &h);
    mode->width = w;
    mode->height = h;
    mode->hz = CBM_SCREEN_HZ;
}

/* ---- Keyboard and joystick ----
 *
 * By position, as VICE maps a PC keyboard: the C64's keys where a PC has
 * them (` is <-, - is +, = is -, [ is @, ] is *, ; is :, ' is ;, the ISO
 * # key is =, \ is POUND), Esc is RUN/STOP, Tab is CTRL, Ctrl and Alt are
 * C=, Home is CLR/HOME, Page Up is RESTORE, Page Down is the up arrow,
 * cursor keys as on the C64 (left and up with SHIFT), F1-F8 (F2, F4, F6,
 * F8 with SHIFT). The numeric keypad is the joystick (8 4 6 2 and the
 * diagonals, 0 or 5 fire) in port 2 (F11 switches to 1); F12 makes the
 * cursor keys the joystick too (Alt: fire).
 *
 * The C128's own keys (rows K0-K2): F9 ESC, F10 HELP, AltGr ALT, End LINE
 * FEED, Scroll Lock NO SCROLL, Caps Lock CAPS LOCK, Print Screen 40/80
 * DISPLAY (the last two lock, as on the C128; 40/80 also picks the screen
 * shown). Its separate cursor keys and keypad aren't mapped: the main
 * ones work in C64 mode too.
 */
#define K(row, col)     ((row) * 8 + (col))
#define NONE            0xFF
enum {
    K_DEL = K(0, 0), K_RETURN = K(0, 1), K_RIGHT = K(0, 2), K_F7 = K(0, 3), K_F1 = K(0, 4), K_F3 = K(0, 5),
    K_F5 = K(0, 6), K_DOWN = K(0, 7), K_LSHIFT = K(1, 7), K_HOME = K(6, 3), K_RSHIFT = K(6, 4),
    K_CTRL = K(7, 2), K_CBM = K(7, 5), K_STOP = K(7, 7), K_SPACE = K(7, 4), K_UPARROW = K(6, 6),
};

typedef struct {
    uint8_t key, with;              /* with: NONE or K_LSHIFT */
} keymap_t;

#define PLAIN(k)        { (k), NONE }
#define SHIFTED(k)      { (k), K_LSHIFT }

static const keymap_t usage_map[0x53] = {
    [0x04] = PLAIN(K(1, 2)), [0x05] = PLAIN(K(3, 4)), [0x06] = PLAIN(K(2, 4)), [0x07] = PLAIN(K(2, 2)),   /* a b c d */
    [0x08] = PLAIN(K(1, 6)), [0x09] = PLAIN(K(2, 5)), [0x0A] = PLAIN(K(3, 2)), [0x0B] = PLAIN(K(3, 5)),   /* e f g h */
    [0x0C] = PLAIN(K(4, 1)), [0x0D] = PLAIN(K(4, 2)), [0x0E] = PLAIN(K(4, 5)), [0x0F] = PLAIN(K(5, 2)),   /* i j k l */
    [0x10] = PLAIN(K(4, 4)), [0x11] = PLAIN(K(4, 7)), [0x12] = PLAIN(K(4, 6)), [0x13] = PLAIN(K(5, 1)),   /* m n o p */
    [0x14] = PLAIN(K(7, 6)), [0x15] = PLAIN(K(2, 1)), [0x16] = PLAIN(K(1, 5)), [0x17] = PLAIN(K(2, 6)),   /* q r s t */
    [0x18] = PLAIN(K(3, 6)), [0x19] = PLAIN(K(3, 7)), [0x1A] = PLAIN(K(1, 1)), [0x1B] = PLAIN(K(2, 7)),   /* u v w x */
    [0x1C] = PLAIN(K(3, 1)), [0x1D] = PLAIN(K(1, 4)),                                                     /* y z */
    [0x1E] = PLAIN(K(7, 0)), [0x1F] = PLAIN(K(7, 3)), [0x20] = PLAIN(K(1, 0)), [0x21] = PLAIN(K(1, 3)),   /* 1 2 3 4 */
    [0x22] = PLAIN(K(2, 0)), [0x23] = PLAIN(K(2, 3)), [0x24] = PLAIN(K(3, 0)), [0x25] = PLAIN(K(3, 3)),   /* 5 6 7 8 */
    [0x26] = PLAIN(K(4, 0)), [0x27] = PLAIN(K(4, 3)),                                                     /* 9 0 */
    [0x28] = PLAIN(K_RETURN),
    [0x29] = PLAIN(K_STOP),         /* Esc */
    [0x2A] = PLAIN(K_DEL),          /* Backspace */
    [0x2B] = PLAIN(K_CTRL),         /* Tab */
    [0x2C] = PLAIN(K_SPACE),
    [0x2D] = PLAIN(K(5, 0)),        /* - -> + */
    [0x2E] = PLAIN(K(5, 3)),        /* = -> - */
    [0x2F] = PLAIN(K(5, 6)),        /* [ -> @ */
    [0x30] = PLAIN(K(6, 1)),        /* ] -> * */
    [0x31] = PLAIN(K(6, 0)),        /* \ -> POUND */
    [0x32] = PLAIN(K(6, 5)),        /* non-US # -> = */
    [0x33] = PLAIN(K(5, 5)),        /* ; -> : */
    [0x34] = PLAIN(K(6, 2)),        /* ' -> ; */
    [0x35] = PLAIN(K(7, 1)),        /* ` -> <- */
    [0x36] = PLAIN(K(5, 7)),        /* , */
    [0x37] = PLAIN(K(5, 4)),        /* . */
    [0x38] = PLAIN(K(6, 7)),        /* / */
    [0x3A] = PLAIN(K_F1), [0x3B] = SHIFTED(K_F1), [0x3C] = PLAIN(K_F3), [0x3D] = SHIFTED(K_F3),
    [0x3E] = PLAIN(K_F5), [0x3F] = SHIFTED(K_F5), [0x40] = PLAIN(K_F7), [0x41] = SHIFTED(K_F7),
    [0x49] = SHIFTED(K_DEL),        /* Insert: INST */
    [0x4A] = PLAIN(K_HOME),
    [0x4B] = PLAIN(CBM_KEY_RESTORE),    /* Page Up */
    [0x4C] = PLAIN(K_DEL),          /* Delete */
    [0x4E] = PLAIN(K_UPARROW),      /* Page Down */
    [0x4F] = PLAIN(K_RIGHT), [0x50] = SHIFTED(K_RIGHT), [0x51] = PLAIN(K_DOWN), [0x52] = SHIFTED(K_DOWN),
};

#define K128(k, col)    (64 + (k) * 8 + (col))
static const struct {
    uint8_t usage, key;
} c128_keys[] = {
    { 0x42, K128(1, 0) },               /* F9: ESC */
    { 0x43, K128(0, 0) },               /* F10: HELP */
    { 0x4D, K128(1, 3) },               /* End: LINE FEED */
    { 0x47, K128(2, 7) },               /* Scroll Lock: NO SCROLL */
    { 0x39, CBM_KEY_CAPS_LOCK },
    { 0x46, CBM_KEY_40_80 },            /* Print Screen */
};

static uint8_t s_count[CBM_KEYS];       /* presses holding each key */
static uint8_t s_held[256][2];          /* per HID usage: the keys it pressed (+1, 0 = none) */
static uint8_t s_modifiers;
static uint8_t s_joy_pad, s_joy_arrows;
static bool s_arrows_joystick;

static void key_ref(uint8_t key, bool down)
{
    if (key == NONE || key >= CBM_KEYS) {
        return;
    }
    bool was = s_count[key] > 0;
    s_count[key] = down ? s_count[key] + 1 : s_count[key] ? s_count[key] - 1 : 0;
    bool now = s_count[key] > 0;
    if (was != now) {
        cbm_key(key, now);
    }
}

static void press(uint8_t usage, const keymap_t *m, bool pressed)
{
    if (pressed) {
        s_held[usage][0] = m->key + 1;
        s_held[usage][1] = m->with == NONE ? 0 : m->with + 1;
        key_ref(m->with, true);
        key_ref(m->key, true);
    } else {
        if (s_held[usage][0]) {
            key_ref(s_held[usage][0] - 1, false);
        }
        if (s_held[usage][1]) {
            key_ref(s_held[usage][1] - 1, false);
        }
        s_held[usage][0] = s_held[usage][1] = 0;
    }
}

static void joystick_update(void)
{
    cbm_joystick(settings.joystick, s_joy_pad | s_joy_arrows);
    cbm_joystick(3 - settings.joystick, 0);
}

static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        [0x59 - 0x59] = CBM_JOY_DOWN | CBM_JOY_LEFT, CBM_JOY_DOWN, CBM_JOY_DOWN | CBM_JOY_RIGHT,
        CBM_JOY_LEFT, CBM_JOY_FIRE, CBM_JOY_RIGHT,
        CBM_JOY_UP | CBM_JOY_LEFT, CBM_JOY_UP, CBM_JOY_UP | CBM_JOY_RIGHT,
        CBM_JOY_FIRE,                   /* 0 */
        CBM_JOY_FIRE,                   /* . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_joy_pad = pressed ? s_joy_pad | b : s_joy_pad & ~b;
    joystick_update();
    return true;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        if (s_arrows_joystick && (bit == 2 || bit == 6)) {
            s_joy_arrows = pressed ? s_joy_arrows | CBM_JOY_FIRE : s_joy_arrows & ~CBM_JOY_FIRE;
            joystick_update();
            return;
        }
        /* LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI (AltGr: the C128's ALT) */
        static const uint8_t keys[8] = { K_CBM, K_LSHIFT, K_CBM, NONE, K_CBM, K_RSHIFT, K_CBM, NONE };
        uint8_t key = keys[bit];
        if (bit == 6 && settings.model == CBM_MODEL_C128) {
            key = K128(2, 0);
        }
        bool was = s_modifiers & (1u << bit);
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        if (was != pressed) {
            key_ref(key, pressed);
        }
        return;
    }
    if (settings.model == CBM_MODEL_C128) {
        for (size_t i = 0; i < sizeof(c128_keys) / sizeof(c128_keys[0]); i++) {
            if (c128_keys[i].usage == usage) {
                keymap_t m = PLAIN(c128_keys[i].key);
                press(usage, &m, pressed);
                return;
            }
        }
    }
    if (usage == 0x44) {                /* F11: joystick port */
        if (pressed) {
            cbm_joystick(settings.joystick, 0);
            settings.joystick = 3 - settings.joystick;
            settings_save();
            joystick_update();
            ESP_LOGI(TAG, "joystick in port %d", settings.joystick);
        }
        return;
    }
    if (usage == 0x45) {                /* F12: cursor keys <-> joystick */
        if (pressed) {
            s_arrows_joystick = !s_arrows_joystick;
            s_joy_arrows = 0;
            joystick_update();
        }
        return;
    }
    if (s_arrows_joystick && usage >= 0x4F && usage <= 0x52) {
        static const uint8_t dir[4] = { CBM_JOY_RIGHT, CBM_JOY_LEFT, CBM_JOY_DOWN, CBM_JOY_UP };
        uint8_t b = dir[usage - 0x4F];
        s_joy_arrows = pressed ? s_joy_arrows | b : s_joy_arrows & ~b;
        joystick_update();
        return;
    }
    if (keypad_joystick(usage, pressed)) {
        return;
    }
    if (usage == 0x58) {                /* keypad Enter */
        press(usage, &usage_map[0x28], pressed);
        return;
    }
    if (usage == 0x64) {                /* non-US \ (ISO key left of Z): POUND */
        press(usage, &usage_map[0x31], pressed);
        return;
    }
    if (usage < sizeof(usage_map) / sizeof(usage_map[0]) &&
        (usage_map[usage].key || usage_map[usage].with)) {
        press(usage, &usage_map[usage], pressed);
    }
}

/* ---- Setup screen ---- */

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < CBM_MODEL_COUNT) {
        settings.model = i;
        settings_save();
    }
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set, false },
};

/* ---- Drives: 8, 9, the cartridge, the program ---- */

enum { DRIVE_8, DRIVE_9, DRIVE_CART, DRIVE_PROGRAM, DRIVES };

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_disk(const char *name)
{
    return has_ext(name, ".d64") || has_ext(name, ".d71") || has_ext(name, ".g64");
}

static bool accepts(int drive, const char *name)
{
    switch (drive) {
    case DRIVE_CART: return has_ext(name, ".crt");
    case DRIVE_PROGRAM: return has_ext(name, ".prg") || has_ext(name, ".t64");
    default: return is_disk(name) && (settings.model == CBM_MODEL_C128 || !has_ext(name, ".d71"));
    }
}

static bool takes_blank(int drive)
{
    return drive <= DRIVE_9;
}

static uint32_t default_size(int drive)
{
    return settings.model == CBM_MODEL_C128 ? CBM_D71_BYTES : CBM_D64_BYTES;
}

static const char *extension_for(uint32_t bytes)
{
    return bytes == CBM_D71_BYTES ? ".d71" : ".d64";
}

static bool insert(int drive, const char *path)
{
    if (!accepts(drive, path)) {
        return false;
    }
    switch (drive) {
    case DRIVE_CART:
        return cbm_cart_insert(path);
    case DRIVE_PROGRAM:
        /* into memory now (the machine must be at READY), or at the next start */
        if (s_started && !cbm_program_load(path, true)) {
            return false;
        }
        snprintf(s_program, sizeof(s_program), "%s", path);
        return true;
    default:
        return cbm_disk_insert(drive, path);
    }
}

static void eject(int drive)
{
    switch (drive) {
    case DRIVE_CART: cbm_cart_eject(); break;
    case DRIVE_PROGRAM: s_program[0] = 0; break;
    default: cbm_disk_eject(drive); break;
    }
}

static const char *path(int drive)
{
    switch (drive) {
    case DRIVE_CART: return cbm_cart_path();
    case DRIVE_PROGRAM: return s_program[0] ? s_program : NULL;
    default: return cbm_disk_path(drive);
    }
}

static bool write_protected(int drive)
{
    return drive > DRIVE_9 || cbm_disk_write_protected(drive);
}

static const char *const extensions[] = { ".d64", ".d71", ".g64", ".crt", ".prg", ".t64", NULL };
static const char *const cart_extensions[] = { ".crt", NULL };     /* in cbm/roms */
static const char *const drive_names[] = { "8:", "9:", "Cartridge", "Program" };
static const disks_size_t sizes[] = {
    { CBM_D64_BYTES, "170 KB 1541 disk (.D64)" },
    { CBM_D71_BYTES, "340 KB 1571 disk (.D71, C128)" },
};

static const disks_config_t disks = {
    .folder = "cbm",
    .extensions = extensions,
    .cart_extensions = cart_extensions,
    .description = "Disks (.D64 .D71 .G64), cartridges, programs",    /* (fits the setup screen) */
    .title = "Disks, cartridge and program",
    .button = "Disks",
    .nvs_namespace = "cbmdisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".d64",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 4 * 1024 * 1024,
    .insert = insert,
    .eject = eject,
    .path = path,
    .write_protected = write_protected,
    .create = cbm_disk_create,
    .accepts = accepts,
    .takes_blank = takes_blank,
    .default_size = default_size,
    .extension_for = extension_for,
};

static void flush(void *arg)
{
    cbm_disk_flush();
}

static void load_program(void *arg)
{
    if (!cbm_program_load(s_program, true)) {
        ESP_LOGW(TAG, "can't load %s", s_program);
    }
}

static void every_second(void)
{
    static int dirty_for;
    if (s_program_wait && --s_program_wait == 0 && s_program[0]) {
        emu_paused(load_program, NULL);
    }
    if (cbm_disk_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ----
 *
 * The machine card's rows: the chips' registers now (cbm_get_chips()).
 * The whole reply has esp32emu's 4 KB: the rows stay short.
 */

/* Pepto's PAL palette (as vic.c draws it) and the VDC's RGBI, for swatches */
static const uint32_t vic_rgb[16] = {
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};
static const uint32_t vdc_rgb[16] = {
    0x000000, 0x555555, 0x0000AA, 0x5555FF, 0x00AA00, 0x55FF55, 0x00AAAA, 0x55FFFF,
    0xAA0000, 0xFF5555, 0xAA00AA, 0xFF55FF, 0xAA5500, 0xFFFF55, 0xAAAAAA, 0xFFFFFF,
};

static void add(char *text, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n >= size - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(text + *n, size - *n, fmt, ap);
    va_end(ap);
    *n = k < 0 ? *n : *n + k >= size ? size - 1 : *n + k;
}

static void swatch(char *text, size_t size, size_t *n, uint32_t rgb)
{
    add(text, size, n, " <b style=\"background:#%06lx\">&nbsp;&nbsp;&nbsp;</b>", (unsigned long)rgb);
}

/* The ROMs, where each came from, and those the CPU sees now */
static void rom_row(emu_json_t *j, const cbm_chips_t *c, bool c128)
{
    static const struct {
        rom_id_t id;
        const char *what;
    } c64[] = {
        { ROM_C64_BASIC, "BASIC V2" }, { ROM_C64_KERNAL, "KERNAL" }, { ROM_C64_CHARGEN, "characters" },
        { ROM_1541, "the 1541-II's DOS" },
    }, c128_roms[] = {
        { ROM_C128_BASIC, "BASIC 7.0" }, { ROM_C128_KERNAL, "KERNAL" }, { ROM_C128_CHARGEN, "characters" },
        { ROM_C128_C64, "the C64 mode's ROM" }, { ROM_1571, "the 1571's DOS" },
    };
    char text[256];
    size_t n = 0;
    int count = c128 ? 5 : 4;
    bool drive = s_cfg.drive_rom != NULL;
    for (int pass = 0; pass < 2; pass++) {      /* those from the SD card, then the flash's */
        int listed = 0;
        for (int i = 0; i < count; i++) {
            rom_id_t id = c128 ? c128_roms[i].id : c64[i].id;
            if ((i == count - 1 && !drive) || rom_store_from_sd(id) != (pass == 0)) {
                continue;
            }
            add(text, sizeof(text), &n, "%s%s", listed++ ? ", " : n ? "; " : "",
                c128 ? c128_roms[i].what : c64[i].what);
        }
        if (listed) {
            add(text, sizeof(text), &n, " from the %s", pass == 0 ? "SD card" : "flash");
        }
    }
    if (!drive) {
        add(text, sizeof(text), &n, "; no drive ROM (no disks)");
    }
    /* in the CPU's map now */
    const char *in[4];
    int k = 0;
    if (c->c128_mode) {
        uint8_t cr = c->mmu[0];
        if (!(cr & 0x02) || ((cr >> 2) & 3) == 0) {
            in[k++] = "BASIC";
        }
        if (((cr >> 4) & 3) == 0) {
            in[k++] = "KERNAL";
            if (cr & 0x01) {
                in[k++] = "characters";
            }
        }
    } else {
        static const char *const names[] = {
            [CBM_MAP_ROML] = "cartridge ROML", [CBM_MAP_ROMH] = "cartridge ROMH", [CBM_MAP_BASIC] = "BASIC",
            [CBM_MAP_KERNAL] = "KERNAL", [CBM_MAP_CHARS] = "characters",
        };
        for (int i = 0; i < 4; i++) {
            if (c->map[i] < sizeof(names) / sizeof(names[0]) && names[c->map[i]]) {
                in[k++] = names[c->map[i]];
            }
        }
    }
    add(text, sizeof(text), &n, "; in now: ");
    for (int i = 0; i < k; i++) {
        add(text, sizeof(text), &n, "%s%s", i ? ", " : "", in[i]);
    }
    if (k == 0) {
        add(text, sizeof(text), &n, "none (all RAM)");
    }
    emu_json_row(j, "ROM", text);
}

static void memory_row(emu_json_t *j, const cbm_chips_t *c)
{
    char text[256];
    size_t n = 0;
    if (c->c128_mode) {
        /* the MMU: CR's banking, common RAM, the page 0 and 1 pointers */
        static const char *const mid[4] = { "BASIC", "function ROM (none)", "function ROM (none)", "RAM" };
        static const char *const top[4] = { "KERNAL", "function ROM (none)", "function ROM (none)", "RAM" };
        static const char *const common_kb[4] = { "1", "4", "8", "16" };
        uint8_t cr = c->mmu[0], rcr = c->mmu[6];
        unsigned hi = (cr >> 4) & 3;
        add(text, sizeof(text), &n, "MMU CR $%02X: RAM bank %u; $4000 %s, $8000 %s, $C000 %s, $D000 %s", cr,
            (cr >> 6) & 1, (cr & 0x02) ? "RAM" : "BASIC", mid[(cr >> 2) & 3], top[hi],
            !(cr & 0x01) ? "I/O" : hi == 0 ? "characters" : top[hi]);
        if (rcr & 0x0C) {
            add(text, sizeof(text), &n, "; %s KB common RAM %s", common_kb[rcr & 3],
                (rcr & 0x0C) == 0x0C ? "at both ends" : (rcr & 0x04) ? "at the bottom" : "at the top");
        }
        if (c->mmu[7] != 0 || (c->mmu[8] & 1) || c->mmu[9] != 1 || (c->mmu[10] & 1)) {
            add(text, sizeof(text), &n, "; pages 0 and 1 at $%02X00, $%02X00 (banks %u, %u)", c->mmu[7],
                c->mmu[9], c->mmu[8] & 1, c->mmu[10] & 1);
        }
    } else {
        /* the 6510's port and the cartridge's lines into the PLA */
        static const char *const names[] = {
            [CBM_MAP_RAM] = "RAM", [CBM_MAP_ROML] = "ROML", [CBM_MAP_ROMH] = "ROMH", [CBM_MAP_BASIC] = "BASIC",
            [CBM_MAP_KERNAL] = "KERNAL", [CBM_MAP_CHARS] = "characters", [CBM_MAP_IO] = "I/O",
            [CBM_MAP_OPEN] = "nothing",
        };
        bool ultimax = c->exrom && !c->game;
        add(text, sizeof(text), &n, "$01 = $%02X: $8000 %s, $A000 %s, $D000 %s, $E000 %s, %s", c->port_data,
            names[c->map[0] & 7], names[c->map[1] & 7], names[c->map[2] & 7], names[c->map[3] & 7],
            ultimax ? "$0000-$0FFF RAM, nothing elsewhere" : "the rest RAM");
        add(text, sizeof(text), &n, "; %s", c->exrom && c->game ? "EXROM and GAME high (no cartridge)" :
            c->game ? "EXROM low (an 8 KB cartridge)" : c->exrom ? "GAME low (Ultimax)" :
            "EXROM and GAME low (a 16 KB cartridge)");
    }
    emu_json_row(j, "Memory", text);
}

/* The VIC-II's mode, where it reads, its colours, its sprites */
static void vic_rows(emu_json_t *j, const cbm_chips_t *c)
{
    static const char *const modes[8] = {
        "Text, 40 × 25 characters, a colour each",
        "Multicolour text, 40 × 25 (4 colours where the colour is 8-15)",
        "Bitmap, 320 × 200, 2 colours a cell of 8 × 8",
        "Multicolour bitmap, 160 × 200, 4 colours a cell",
        "Extended colour text, 40 × 25",
        "Invalid mode (black)", "Invalid mode (black)", "Invalid mode (black)",
    };
    const uint8_t *v = c->vic;
    unsigned mode = ((v[0x11] >> 4) & 6) | ((v[0x16] >> 4) & 1);
    unsigned base = c->vic_bank * 0x4000u, screen = base + ((v[0x18] >> 4) & 15) * 0x400u;
    char text[300];
    size_t n = 0;
    add(text, sizeof(text), &n, "%s; ", modes[mode]);
    if (mode & 2) {
        add(text, sizeof(text), &n, "bitmap at $%04X, colours at $%04X", base + (v[0x18] & 8) * 0x400u, screen);
    } else {
        unsigned chars = base + ((v[0x18] >> 1) & 7) * 0x800u;
        bool rom = c->rom_chars && (chars & 0x3000) == 0x1000 && (!(c->vic_bank & 1) || c->rom_chars_all_banks);
        add(text, sizeof(text), &n, "screen at $%04X, characters at $%04X%s", screen, chars, rom ? " (ROM)" : "");
    }
    if (c->vic_ram_bank) {
        add(text, sizeof(text), &n, " of RAM bank 1");
    }
    add(text, sizeof(text), &n, "; %sscroll %u, %u%s", (v[0x11] & 8) ? (v[0x16] & 8) ? "" : "38 columns, " :
        (v[0x16] & 8) ? "24 rows, " : "24 rows of 38 columns, ", v[0x16] & 7, v[0x11] & 7,
        (v[0x11] & 0x10) ? "" : "; blanked (DEN off)");
    emu_json_row(j, "Screen", text);

    /* the colour registers the mode uses: border, backgrounds */
    n = 0;
    add(text, sizeof(text), &n, "Border");
    swatch(text, sizeof(text), &n, vic_rgb[v[0x20] & 15]);
    int backgrounds = mode == 1 ? 3 : mode == 4 ? 4 : 1;
    add(text, sizeof(text), &n, ", background%s", backgrounds > 1 ? "s" : "");
    for (int i = 0; i < backgrounds; i++) {
        swatch(text, sizeof(text), &n, vic_rgb[v[0x21 + i] & 15]);
    }
    emu_json_row(j, "Palette", text);

    n = 0;
    for (int i = 0; i < 8; i++) {
        if (v[0x15] & (1 << i)) {
            add(text, sizeof(text), &n, "%s%d at %u,%u", n ? ", " : "", i, v[2 * i] | (((v[0x10] >> i) & 1) << 8),
                v[2 * i + 1]);
        }
    }
    if (n == 0) {
        add(text, sizeof(text), &n, "None on");
    }
    static const struct {
        uint8_t reg;
        const char *what;
    } flags[] = { { 0x1C, "multicolour" } };
    for (size_t f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
        uint8_t on = v[flags[f].reg] & v[0x15];
        if (on) {
            add(text, sizeof(text), &n, "; %s", flags[f].what);
            for (int i = 0; i < 8; i++) {
                if (on & (1 << i)) {
                    add(text, sizeof(text), &n, " %d", i);
                }
            }
        }
    }
    emu_json_row(j, "Sprites", text);
}

/* The C128's 80 columns (shown instead of the VIC-II's 40 while 40/80 is down) */
static void vdc_rows(emu_json_t *j, const cbm_chips_t *c)
{
    const uint8_t *r = c->vdc;
    char text[200];
    size_t n = 0;
    unsigned start = (r[12] << 8) | r[13];
    add(text, sizeof(text), &n, "80 columns (the VDC, %u KB): ", (r[28] & 0x10) ? 64 : 16);
    if (r[25] & 0x80) {
        add(text, sizeof(text), &n, "bitmap, %u × %u at $%04X", r[1] * 8u, r[6] * ((r[9] & 31) + 1u), start);
    } else {
        add(text, sizeof(text), &n, "text, %u × %u at $%04X, characters at $%04X", r[1], r[6], start,
            (r[28] & 0xE0) << 8);
    }
    if (r[25] & 0x40) {
        add(text, sizeof(text), &n, ", attributes at $%04X", (r[20] << 8) | r[21]);
    }
    emu_json_row(j, "Screen", text);
    n = 0;
    if (r[25] & 0x40) {
        add(text, sizeof(text), &n, "Background");       /* the colours: the attributes' */
    } else {
        add(text, sizeof(text), &n, "Foreground");
        swatch(text, sizeof(text), &n, vdc_rgb[r[26] >> 4]);
        add(text, sizeof(text), &n, ", background");
    }
    swatch(text, sizeof(text), &n, vdc_rgb[r[26] & 15]);
    emu_json_row(j, "Palette", text);
}

static void sound_row(emu_json_t *j, const cbm_chips_t *c)
{
    static const char *const waves[16] = {
        "", "triangle", "sawtooth", "triangle+sawtooth", "pulse", "pulse+triangle", "pulse+sawtooth",
        "combined waves", "noise", "noise+triangle", "noise+sawtooth", "combined waves", "noise+pulse",
        "combined waves", "combined waves", "combined waves",
    };
    const uint8_t *s = c->sid;
    char text[300];
    size_t n = 0;
    add(text, sizeof(text), &n, "Volume %u", s[0x18] & 15);
    if ((s[0x18] & 0x70) && (s[0x17] & 0x0F)) {
        static const char *const passes[8] = {
            "", "low-pass", "band-pass", "low+band-pass", "high-pass", "notch", "band+high-pass", "all-pass",
        };
        add(text, sizeof(text), &n, "; %s filter at $%03X, resonance %u, on", passes[(s[0x18] >> 4) & 7],
            (s[0x15] & 7) | (s[0x16] << 3), s[0x17] >> 4);
        for (int i = 0; i < 4; i++) {
            if (s[0x17] & (1 << i)) {
                add(text, sizeof(text), &n, i < 3 ? " %d" : " ext", i + 1);
            }
        }
    }
    int sounding = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t *v = s + 7 * i;
        uint8_t ctrl = v[4], wave = ctrl >> 4;
        bool on = c->sound ? c->sid_level[i] > 0 : (ctrl & 1);
        if (!wave || !on || (ctrl & 0x08) || (i == 2 && (s[0x18] & 0x80) && !(s[0x17] & 4))) {
            continue;
        }
        unsigned freq = v[0] | (v[1] << 8), pw = v[2] | ((v[3] & 15) << 8);
        add(text, sizeof(text), &n, "; voice %d: %lu Hz %s", i + 1,
            (unsigned long)(((uint64_t)freq * CBM_CLOCK_HZ + (1u << 23)) >> 24), waves[wave]);
        if (wave & 4) {
            add(text, sizeof(text), &n, " %u%%", pw * 100 / 4096);
        }
        add(text, sizeof(text), &n, "%s%s", (ctrl & 4) ? ", ring" : "", (ctrl & 2) ? ", sync" : "");
        if (c->sound) {
            add(text, sizeof(text), &n, ", level %u", c->sid_level[i]);
        }
        sounding++;
    }
    if (!sounding) {
        add(text, sizeof(text), &n, ", silent");
    }
    emu_json_row(j, "Sound", text);
}

/* The VIC's interrupts and the CIAs' timers (CIA 1 the IRQ, CIA 2 the NMI) */
static void interrupts_row(emu_json_t *j, const cbm_chips_t *c)
{
    char text[300];
    size_t n = 0;
    const uint8_t *v = c->vic;
    add(text, sizeof(text), &n, "VIC:");
    if (v[0x1A] & 1) {
        add(text, sizeof(text), &n, " raster line %u", v[0x12] | ((v[0x11] & 0x80) << 1));
    }
    if (v[0x1A] & 6) {
        add(text, sizeof(text), &n, "%s sprite collisions", (v[0x1A] & 1) ? "," : "");
    }
    if (!(v[0x1A] & 7)) {
        add(text, sizeof(text), &n, " none");
    }
    for (int i = 0; i < 2; i++) {
        add(text, sizeof(text), &n, "; CIA %d:", i + 1);
        int timers = 0;
        for (int t = 0; t < 2; t++) {
            uint8_t cr = c->cia[i].cr[t];
            if (!(cr & 1)) {
                continue;
            }
            unsigned latch = c->cia[i].latch[t];
            bool phi2 = t == 0 ? !(cr & 0x20) : !(cr & 0x60);
            add(text, sizeof(text), &n, "%s %c $%04X", timers++ ? "," : " timer", 'A' + t, latch);
            if (cr & 0x08) {
                add(text, sizeof(text), &n, " once");
            } else if (phi2) {
                unsigned long hz = (CBM_CLOCK_HZ + (latch + 1ul) / 2) / (latch + 1ul);
                add(text, sizeof(text), &n, hz < 10000 ? " (%lu Hz)" : " (%lu kHz)", hz < 10000 ? hz : hz / 1000);
            }
            if (c->cia[i].imr & (1 << t)) {
                add(text, sizeof(text), &n, i ? " NMI" : " IRQ");
            }
        }
        if (!timers) {
            add(text, sizeof(text), &n, " stopped");
        }
    }
    emu_json_row(j, "Interrupts", text);
}

static void status(emu_json_t *j, int section)
{
    char text[300];
    if (section == EMU_STATUS_ROWS) {
        cbm_chips_t c;
        cbm_get_chips(&c);
        bool c128 = cbm_model() == CBM_MODEL_C128;
        if (c128) {
            snprintf(text, sizeof(text), "Commodore 128 (PAL), %s mode: %s, 128 KB RAM, VIC-II, VDC, SID 6581, "
                     "two CIAs", c.c128_mode ? "C128" : "C64", c.z80 ? "the Z80 has the bus" :
                     c.fast ? "an 8502 at 2 MHz (FAST)" : "an 8502 at 1 MHz");
        } else {
            snprintf(text, sizeof(text), "Commodore 64 (PAL): a 6510 at 0.985 MHz, 64 KB RAM, VIC-II, SID 6581, "
                     "two 6526 CIAs");
        }
        emu_json_row(j, "System", text);
        rom_row(j, &c, c128);
        memory_row(j, &c);
        if (c.c128_mode && c.key_40_80) {
            vdc_rows(j, &c);
        } else {
            vic_rows(j, &c);
        }
        sound_row(j, &c);
        interrupts_row(j, &c);
        size_t n = 0;
        add(text, sizeof(text), &n, "By position (as VICE); Esc RUN/STOP, Tab CTRL, Ctrl and Alt C=, Page Up "
            "RESTORE, Page Down ↑");
        if (c128) {
            add(text, sizeof(text), &n, "; F9 ESC, F10 HELP, AltGr ALT, End LINE FEED, Scroll Lock NO SCROLL, "
                "Caps Lock (%s), Print Screen 40/80 DISPLAY", c.caps_lock ? "on" : "off");
        }
        emu_json_row(j, "Keyboard", text);
        uint8_t joy = s_joy_pad | s_joy_arrows;
        n = 0;
        add(text, sizeof(text), &n, "Port %d: keypad (0, 5 fire)%s; F11 swaps ports, F12 the cursor keys",
            settings.joystick, s_arrows_joystick ? " and cursor keys (Alt fire)" : "");
        if (joy) {
            static const char *const dirs[5] = { "up", "down", "left", "right", "fire" };
            add(text, sizeof(text), &n, "; now:");
            for (int i = 0; i < 5; i++) {
                if (joy & (1 << i)) {
                    add(text, sizeof(text), &n, " %s", dirs[i]);
                }
            }
        }
        emu_json_row(j, "Joystick", text);
        emu_json_row(j, "Disks", c128 ? "8: and 9: 1571s, emulated whole: D64, D71, G64; cartridges CRT; "
                     "programs PRG, T64" : "8: and 9: 1541-IIs, emulated whole: D64, G64; cartridges CRT; "
                     "programs PRG, T64");
        return;
    }
    cbm_disk_status_t ds;
    cbm_disk_status(&ds);
    for (int d = DRIVE_8; d <= DRIVE_9; d++) {
        const char *name = disks_name(d);
        uint64_t free_bytes = 0, size = 0;
        cbm_disk_space(d, &free_bytes, &size);
        snprintf(text, sizeof(text), "track %u%s", ds.track[d], ds.motor[d] ? ", motor on" : "");
        emu_json_drive_space(j, drive_names[d], name, cbm_disk_write_protected(d), ds.light == d, ds.reads[d],
                             ds.writes[d], name ? text : NULL, free_bytes, size);
    }
    const char *cart = disks_name(DRIVE_CART);
    emu_json_drive(j, drive_names[DRIVE_CART], cart, true, false, 0, 0, cart ? cbm_cart_info() : NULL);
    emu_json_drive(j, drive_names[DRIVE_PROGRAM], disks_name(DRIVE_PROGRAM), true, false, 0, 0, NULL);
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    cbm_status_t s;
    cbm_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%04X $01=%02X drive 8 track %u%s", (unsigned long)s.frames, s.pc,
             s.port01, s.drive_track[0], s.drive_motor[0] ? " motor" : "");
}

/*   k <n>              tap key number n (row * 8 + column, e.g. 1 RETURN)
 *   K <n> <0|1>        key n down (1) or up (0)
 *   T <text>           type text (\n is RETURN)
 *   j <port> <bits>    joystick (1 up 2 down 4 left 8 right 16 fire)
 *   R <model>          restart as model 0 C64, 1 C128
 */
static bool console_command(const char *line)
{
    switch (line[0]) {

    case 'k': {
        unsigned key = strtoul(line + 1, NULL, 10);
        cbm_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        cbm_key(key, false);
        return true;
    }
    case 'K': {
        unsigned key = 0, down = 0;
        if (sscanf(line + 1, "%u %u", &key, &down) == 2) {
            cbm_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[200];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        cbm_type(text);
        return true;
    }
    case 'j': {
        int port = 2;
        unsigned bits = 0;
        sscanf(line + 1, "%d %u", &port, &bits);
        cbm_joystick(port, bits);
        return true;
    }
    case 'R': {
        int model = 0;
        if (sscanf(line + 1, "%d", &model) == 1 && model >= 0 && model < CBM_MODEL_COUNT) {
            settings.model = model;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Commodore 64/128",
    .short_name = "Commodore",
    .view_width = CBM_SCREEN_WIDTH * 2, /* 400 x 300, doubled: 800 x 600 */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = cbm_reset,
    .run_frame = cbm_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = cbm_screen_signature,
    .render_line = cbm_render_line_rgb565,
    .audio_hz = CBM_AUDIO_HZ,
    .audio_enable = cbm_audio_enable,
    .audio_render = cbm_audio_render,
    .audio_pending = cbm_audio_pending,
    .hid_key = hid_key,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
