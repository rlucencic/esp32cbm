/*
 * Desktop build of the Commodore core, for testing without the board:
 *
 *   cbm_host <c64|c128> [frames] [out.ppm]
 *
 * ROMs from CBM_ROMS (default ../../../roms): c64basic.rom c64kernal.rom
 * c64chargen.rom 1541.rom; c128basic.rom c128kernal.rom c128chargen.rom
 * c128c64.rom 1571.rom.
 *
 * Environment:
 *   CBM_DISK8, CBM_DISK9    disk images (.d64 .d71 .g64)
 *   CBM_NEWDISK=<path>      make a blank disk first (.d71: double-sided)
 *   CBM_CART=<file>         cartridge (.crt)
 *   CBM_PRG=<file>          program (.prg .t64), loaded at frame 150 and RUN
 *   CBM_JOY=<port>          joystick port the script's "joy" uses (default 2)
 *   CBM_SCRIPT=<file>       lines "<frame> <command>":
 *                             type <text>          type it (\n for RETURN)
 *                             key <n> 0|1          key number down / up
 *                             joy <bits>           joystick
 *                             shot <file.ppm>      screenshot
 *                             insert <d> <path>    disk into drive d (0: 8, 1: 9)
 *                             eject <d>
 *                             cart <path> | prg <path>
 *                             peek <hex addr> | dump <file> (64 KB as the CPU
 *                             sees it) | dumpram <file> (all RAM, 128 KB on a
 *                             C128) | text [hex addr] (the text screen, from
 *                             $0400) | reset
 *                             z80io 0|1, z80watch <hex addr>: with
 *                             -DCBM_Z80IO_LOG, the Z80's keyboard I/O and its
 *                             writes to 8 bytes from addr
 *   CBM_STATUS=<n>          print PC (the Z80's while it runs), drive 8 and the
 *                           instructions run per frame every n frames
 *   CBM_TRACE=<pc>[:<n>]    disassemble n instructions from when PC is reached
 *   CBM_TRACE_FRAME=<f>     ... from frame f on
 *   CBM_Z80TRACE=<n>        the Z80's first n instructions (C128)
 *   CBM_Z80TRACE_PC=<pc>[:<f>]  ... from that address (and frame) on
 *   CBM_WAV=<file>          record the sound (stereo WAV)
 *   CBM_QUIET               no summary
 *   CBM_DEBUG=<port>        serve the debugger (esp32emu's docs/debugger.md) on
 *                           127.0.0.1:<port> and run in real time until killed
 *
 * Other compile-time logs: -DCBM_MMU_LOG (C128 MMU writes, CPU switches),
 * -DCBM_FAST_LOG (fast serial bytes, 1571 side and speed changes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cbm.h"
#include "m6502.h"
#include "z80.h"
#include "emudbg.h"

static uint16_t s_frame[CBM_SCREEN_WIDTH * CBM_SCREEN_HEIGHT];
static uint16_t s_vdc[CBM_VDC_WIDTH * CBM_VDC_HEIGHT];

static uint8_t *s_cfg_ram;

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    int w, h;
    cbm_screen_mode(&w, &h);
    int sx = w == CBM_SCREEN_WIDTH ? 2 : 1;
    fprintf(f, "P6\n%d %d\n255\n", w * sx, h * 2);
    uint16_t line[CBM_VDC_WIDTH];
    uint8_t rgb[CBM_VDC_WIDTH * 2 * 3];
    for (int y = 0; y < h; y++) {
        cbm_render_line_rgb565(y, line);
        for (int x = 0; x < w; x++) {
            uint16_t c = line[x];
            for (int k = 0; k < sx; k++) {
                rgb[(x * sx + k) * 3] = ((c >> 11) & 31) * 255 / 31;
                rgb[(x * sx + k) * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
                rgb[(x * sx + k) * 3 + 2] = (c & 31) * 255 / 31;
            }
        }
        fwrite(rgb, 1, w * sx * 3, f);
        fwrite(rgb, 1, w * sx * 3, f);
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

static uint8_t *read_file(const char *dir, const char *name, uint32_t *size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (size) {
        *size = n;
    }
    return buf;
}

/* ---- Tracing ---- */

static uint16_t s_trace_pc;
static int s_trace_left;
static bool s_tracing, s_trace_on;
static uint32_t s_trace_frame, s_frame_now;
static const m6502_t *s_main_cpu;

static int s_z80_trace = -1;            /* CBM_Z80TRACE=n: the first n Z80 instructions */

void z80_host_hook(void)
{
    static int start_pc = -1, start_frame;
    static bool armed;
    if (s_z80_trace < 0) {
        s_z80_trace = getenv("CBM_Z80TRACE") ? atoi(getenv("CBM_Z80TRACE")) : 0;
        /* CBM_Z80TRACE_PC=<hex>[:frame]: from there on */
        if (getenv("CBM_Z80TRACE_PC")) {
            sscanf(getenv("CBM_Z80TRACE_PC"), "%x:%d", (unsigned *)&start_pc, &start_frame);
        }
        armed = start_pc < 0;
    }
    if (!armed) {
        if (z80.pc.w != start_pc || (int)s_frame_now < start_frame) {
            return;
        }
        armed = true;
    }
    if (s_z80_trace > 0) {
        s_z80_trace--;
        char buf[48];
        z80_disassemble(z80.pc.w, buf, sizeof(buf));
        printf("  Z80 %04X  %-20s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X\n", z80.pc.w, buf, z80.af.w, z80.bc.w,
               z80.de.w, z80.hl.w, z80.sp.w);
    }
}

static uint64_t s_count_main, s_count_other;     /* instructions (CBM_COUNT) */

void m6502_host_hook(m6502_t *c)
{
    if (c == s_main_cpu) {
        s_count_main++;
    } else {
        s_count_other++;
    }
    if (!s_trace_on || c != s_main_cpu) {
        return;
    }
    if (!s_tracing && c->pc == s_trace_pc && s_trace_left > 0 && s_frame_now >= s_trace_frame) {
        s_tracing = true;
    }
    if (s_tracing) {
        char buf[64];
        m6502_disassemble(c, c->pc, buf, sizeof(buf));
        printf("  %04X  %-16s A=%02X X=%02X Y=%02X S=%02X P=%02X cyc=%llu\n", c->pc, buf, c->a, c->x, c->y,
               c->s, c->p, (unsigned long long)c->cycles);
        if (--s_trace_left <= 0) {
            s_tracing = false;
        }
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[512];
} line_t;

static line_t *s_script;
static int s_lines, s_next;
static int s_joy_port = 2;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "no script %s\n", path);
        exit(1);
    }
    s_script = calloc(4096, sizeof(line_t));
    char buf[600];
    while (s_lines < 4096 && fgets(buf, sizeof(buf), f)) {
        line_t *l = &s_script[s_lines];
        int at = 0;
        if (buf[0] == '#' || sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &at) < 2) {
            continue;
        }
        snprintf(l->arg, sizeof(l->arg), "%s", buf + at);
        l->arg[strcspn(l->arg, "\r\n")] = 0;
        s_lines++;
    }
    fclose(f);
}

static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s++;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void run_script(uint32_t frame)
{
    while (s_next < s_lines && s_script[s_next].frame <= frame) {
        line_t *l = &s_script[s_next++];
        if (strcmp(l->cmd, "type") == 0) {
            unescape(l->arg);
            cbm_type(l->arg);
        } else if (strcmp(l->cmd, "key") == 0) {
            unsigned key = 0, down = 0;
            sscanf(l->arg, "%u %u", &key, &down);
            cbm_key(key, down);
        } else if (strcmp(l->cmd, "joy") == 0) {
            cbm_joystick(s_joy_port, strtoul(l->arg, NULL, 0));
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg);
        } else if (strcmp(l->cmd, "insert") == 0) {
            int d = 0, at = 0;
            sscanf(l->arg, "%d %n", &d, &at);
            if (!cbm_disk_insert(d, l->arg + at)) {
                printf("host: can't insert %s\n", l->arg + at);
            }
        } else if (strcmp(l->cmd, "eject") == 0) {
            cbm_disk_eject(atoi(l->arg));
        } else if (strcmp(l->cmd, "cart") == 0) {
            if (!cbm_cart_insert(l->arg)) {
                printf("host: can't use %s\n", l->arg);
            }
        } else if (strcmp(l->cmd, "prg") == 0) {
            if (!cbm_program_load(l->arg, true)) {
                printf("host: can't load %s\n", l->arg);
            }
        } else if (strcmp(l->cmd, "peek") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: $%04X = $%02X\n", frame, a, cbm_peek(a));
        } else if (strcmp(l->cmd, "text") == 0) {
            /* the text screen (at $0400 or the address given) as ASCII */
            unsigned a = 0x400;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: screen\n", frame);
            for (int row = 0; row < 25; row++) {
                char s[41];
                for (int col = 0; col < 40; col++) {
                    uint8_t c = cbm_peek((a + row * 40 + col) & 0xFFFF) & 0x7F;
                    s[col] = c == 0 ? '@' : c < 27 ? 'a' + c - 1 : c < 32 ? "[\\]^_"[c - 27] :
                             c < 64 ? c : c < 91 ? 'A' + c - 65 : '.';
                }
                int n = 40;
                while (n > 0 && s[n - 1] == ' ') {
                    n--;
                }
                printf("| %.*s\n", n, s);
            }
        } else if (strcmp(l->cmd, "z80watch") == 0) {
#ifdef CBM_Z80IO_LOG
            extern int c128_z80_watch;
            sscanf(l->arg, "%x", (unsigned *)&c128_z80_watch);
#endif
        } else if (strcmp(l->cmd, "z80io") == 0) {
#ifdef CBM_Z80IO_LOG
            extern bool c128_z80io_log;
            c128_z80io_log = atoi(l->arg);
#endif
        } else if (strcmp(l->cmd, "dumpram") == 0) {
            /* all of RAM as it is (the C128's 128 KB) */
            FILE *df = fopen(l->arg, "wb");
            if (df) {
                fwrite(s_cfg_ram, 1, cbm_model() == CBM_MODEL_C128 ? 0x20000 : 0x10000, df);
                fclose(df);
            }
        } else if (strcmp(l->cmd, "dump") == 0) {
            FILE *df = fopen(l->arg, "wb");
            for (int a = 0; df && a < 65536; a++) {
                fputc(cbm_peek(a), df);
            }
            if (df) {
                fclose(df);
            }
        } else if (strcmp(l->cmd, "reset") == 0) {
            cbm_reset();
        } else {
            printf("host: unknown script command %s\n", l->cmd);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <c64|c128> [frames] [out.ppm]\n", argv[0]);
        return 1;
    }
    int model = strcmp(argv[1], "c128") == 0 ? CBM_MODEL_C128 : strcmp(argv[1], "c64") == 0 ? CBM_MODEL_C64 : -1;
    if (model < 0) {
        fprintf(stderr, "unknown model %s\n", argv[1]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 250;
    const char *out = argc > 3 ? argv[3] : NULL;
    const char *dir = getenv("CBM_ROMS") ? getenv("CBM_ROMS") : "../../../roms";
    bool c128 = model == CBM_MODEL_C128;
    cbm_config_t cfg = {
        .model = model,
        .ram = s_cfg_ram = malloc(128 * 1024),
        .basic = read_file(dir, c128 ? "c128basic.rom" : "c64basic.rom", NULL),
        .kernal = read_file(dir, c128 ? "c128kernal.rom" : "c64kernal.rom", NULL),
        .chargen = read_file(dir, c128 ? "c128chargen.rom" : "c64chargen.rom", NULL),
        .c64_rom = c128 ? read_file(dir, "c128c64.rom", NULL) : NULL,
        .drive_rom = read_file(dir, c128 ? "1571.rom" : "1541.rom", NULL),
        .frame = s_frame,
        .vdc_frame = s_vdc,
    };
    if (cfg.basic == NULL || cfg.kernal == NULL || cfg.chargen == NULL) {
        fprintf(stderr, "ROMs missing in %s\n", dir);
        return 1;
    }
    FILE *wav = getenv("CBM_WAV") ? fopen(getenv("CBM_WAV"), "wb") : NULL;
    cbm_audio_enable(wav != NULL);
    if (getenv("CBM_JOY")) {
        s_joy_port = atoi(getenv("CBM_JOY"));
    }
    if (getenv("CBM_NEWDISK")) {
        const char *p = getenv("CBM_NEWDISK");
        size_t n = strlen(p);
        bool d71 = n > 4 && strcmp(p + n - 4, ".d71") == 0;
        if (!cbm_disk_create(p, d71 ? CBM_D71_BYTES : CBM_D64_BYTES)) {
            fprintf(stderr, "can't make %s\n", p);
        }
    }
    if (!cbm_init(&cfg)) {
        fprintf(stderr, "cbm_init failed\n");
        return 1;
    }
    for (int d = 0; d < CBM_DRIVES; d++) {
        const char *path = getenv(d ? "CBM_DISK9" : "CBM_DISK8");
        if (path && !cbm_disk_insert(d, path)) {
            fprintf(stderr, "can't insert %s\n", path);
        }
    }
    if (getenv("CBM_CART") && !cbm_cart_insert(getenv("CBM_CART"))) {
        fprintf(stderr, "can't use %s\n", getenv("CBM_CART"));
    }
    if (getenv("CBM_SCRIPT")) {
        load_script(getenv("CBM_SCRIPT"));
    }
    extern m6502_t *cbm_main_cpu(void);
    s_main_cpu = cbm_main_cpu();
    emudbg_host_run("CBM_DEBUG", cbm_run_frame, 50);
    const char *tr = getenv("CBM_TRACE");
    if (tr) {
        unsigned pc = 0;
        s_trace_left = 200;
        sscanf(tr, "%x:%d", &pc, &s_trace_left);
        s_trace_pc = pc;
        s_trace_on = true;
        s_trace_frame = getenv("CBM_TRACE_FRAME") ? atoi(getenv("CBM_TRACE_FRAME")) : 0;
    }
    int status_every = getenv("CBM_STATUS") ? atoi(getenv("CBM_STATUS")) : 0;
    uint32_t wav_frames = 0;
    if (wav) {
        fseek(wav, 44, SEEK_SET);
    }

    clock_t start = clock();
    for (int f = 0; f < frames; f++) {
        s_frame_now = f;
        run_script(f);
        if (f == 150 && getenv("CBM_PRG") && !cbm_program_load(getenv("CBM_PRG"), true)) {
            fprintf(stderr, "can't load %s\n", getenv("CBM_PRG"));
        }
        cbm_run_frame();
        if (wav) {
            int16_t buf[1024 * 2];
            int n;
            while ((n = cbm_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, n, wav);
                wav_frames += n;
            }
        }
        if (status_every && (f + 1) % status_every == 0) {
            cbm_status_t s;
            cbm_get_status(&s);
            printf("frame %d: PC $%04X $01=%02X raster %u%s%s drive8 %s track %u PC $%04X\n", f + 1, s.pc,
                   s.port01, s.raster, s.z80 ? " Z80" : "", s.fast ? " 2MHz" : "", s.drive_motor[0] ? "motor" : "-",
                   s.drive_track[0], s.drive_pc[0]);
            printf("  instructions per frame: computer %llu, drives %llu\n",
                   (unsigned long long)(s_count_main / status_every), (unsigned long long)(s_count_other / status_every));
            s_count_main = s_count_other = 0;
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (cbm_disk_dirty()) {
        cbm_disk_flush();
    }
    if (!getenv("CBM_QUIET")) {
        cbm_status_t s;
        cbm_get_status(&s);
        printf("%d frames in %.2f s (%.0f%% of real time), PC $%04X\n", frames, secs,
               frames / 50.125 / secs * 100, s.pc);
        for (int d = 0; d < CBM_DRIVES; d++) {
            uint64_t free_bytes, size;
            if (cbm_disk_space(d, &free_bytes, &size)) {
                printf("%d: %llu bytes free of %llu (%llu blocks free)\n", 8 + d, (unsigned long long)free_bytes,
                       (unsigned long long)size, (unsigned long long)free_bytes / 256);
            }
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (wav) {
        uint32_t data = wav_frames * 4, rate = CBM_AUDIO_HZ, bytes = rate * 4;
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data...";
        uint32_t riff = 36 + data;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &bytes, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
