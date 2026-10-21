/*
 * Commodore 64 and 128 emulator on the Makerfabs MaTouch ESP32-S3 7".
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Commodore into it: memory, ROMs, settings, keyboard map, joystick,
 * drives 8 and 9, the cartridge and program slots, status.
 */
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
             "No %s ROM: put %s into roms/ and flash with idf.py flash, or copy it into the "
             "\"roms\" folder on the SD card.", model_full[settings.model], what);
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
static const char *const drive_names[] = { "8:", "9:", "Cartridge", "Program" };
static const disks_size_t sizes[] = {
    { CBM_D64_BYTES, "170 KB 1541 disk (.D64)" },
    { CBM_D71_BYTES, "340 KB 1571 disk (.D71, C128)" },
};

static const disks_config_t disks = {
    .folder = "cbm",
    .extensions = extensions,
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

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        emu_json_row(j, "System", model_full[cbm_model()]);
        emu_json_row(j, "ROMs", s_rom_text);
        snprintf(text, sizeof(text), "port %d on the numeric keypad%s (F11 switches)", settings.joystick,
                 s_arrows_joystick ? " and cursor keys" : "");
        emu_json_row(j, "Joystick", text);
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
