#!/usr/bin/env python3
"""Check actual VIC character expansion against a per-pixel oracle.

Extracts production helpers; stubs g_byte to isolate arithmetic and verifies
its call arguments/count. Optional --reference compares an older vic.c too.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEAD = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct input { uint8_t data, c, color, mode, bg[4]; bool display; int col; uint16_t cb; };
struct output { uint64_t px, fg; };
'''
STATE = r'''
static uint64_t s_mask[256];
#define REP(c) ((uint64_t)(c) * 0x0101010101010101ULL)
static uint8_t s_vm[40], s_col[40];
static bool s_display;
static const struct input *current;
static unsigned reads;
static uint8_t g_byte(int col, uint8_t mode, uint16_t cb) {
    assert(col == current->col && mode == current->mode && cb == current->cb);
    reads++;
    return current->data;
}
'''
TAIL = r'''
void NAME(const struct input *in, struct output *out) {
    static bool initialized;
    if (!initialized) { init_masks(); initialized = true; }
    uint64_t guarded[4] = {0x9876543210abcdefULL, 0, 0, 0x13579bdf2468ace0ULL};
    current = in; reads = 0;
    s_vm[in->col] = in->c; s_col[in->col] = in->color; s_display = in->display;
    char_pixels(in->col, in->mode, in->bg, in->cb, &guarded[1], &guarded[2]);
    assert(reads == 1);
    assert(guarded[0] == 0x9876543210abcdefULL && guarded[3] == 0x13579bdf2468ace0ULL);
    out->px = guarded[1]; out->fg = guarded[2];
}
'''
TEST = r'''
void candidate(const struct input *, struct output *);
#ifdef WITH_REFERENCE
void reference(const struct input *, struct output *);
#endif
static uint32_t seed = 0x93a17c25;
static unsigned cases;
static uint32_t random_word(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed;
}
static struct output oracle(const struct input *in) {
    struct output out = {0, 0};
    for (int x = 0; x < 8; x++) {
        unsigned pixel = 0, foreground;
        bool invalid = !in->display || in->mode >= 5;
        bool multi = (in->mode & 1) && ((in->mode & 2) || (in->color & 8));
        unsigned pair = (in->data >> (6 - 2 * (x / 2))) & 3;
        unsigned bit = (in->data >> (7 - x)) & 1;
        if (invalid) {
            foreground = (in->mode & 1) ? (pair / 2) : bit;
        } else if (multi) {
            unsigned colors[4] = {in->bg[0], in->bg[1], in->bg[2], in->color & 7};
            if (in->mode & 2) { colors[1] = in->c / 16; colors[2] = in->c % 16; colors[3] = in->color & 15; }
            pixel = colors[pair]; foreground = pair / 2;
        } else {
            unsigned on = in->color & 15, off = in->bg[0];
            if (in->mode == 2 || in->mode == 3) {on = in->c / 16; off = in->c % 16;}
            else if (in->mode == 4) off = in->bg[in->c / 64];
            else if (in->mode == 1) on = in->color & 7;
            pixel = bit ? on : off; foreground = bit;
        }
        out.px |= (uint64_t)pixel << (8 * x);
        out.fg |= (uint64_t)foreground << (8 * x);
    }
    return out;
}
static void check(const struct input *in) {
    struct output got, expected = oracle(in);
    candidate(in, &got);
    if (got.px != expected.px || got.fg != expected.fg) {
        fprintf(stderr, "mismatch case %u data %u mode %u color %u display %d\n",cases,in->data,in->mode,in->color,in->display); assert(0);
    }
#ifdef WITH_REFERENCE
    struct output old; reference(in, &old); assert(got.px == old.px && got.fg == old.fg);
#endif
    cases++;
}
int main(void) {
    struct input in = {.display = true};
    /* Exhaust every data byte, screen byte, valid color and mode, both display states. */
    for (unsigned data = 0; data < 256; data++)
    for (unsigned c = 0; c < 256; c++)
    for (unsigned color = 0; color < 16; color++)
    for (unsigned mode = 0; mode < 8; mode++) {
        in.data=data; in.c=c; in.color=color; in.mode=mode;
        for (int i=0;i<4;i++) in.bg[i]=(data+c+color+i*5)&15;
        in.col=(data+c+color+mode)%40; in.cb=(c&7)*2048;
        in.display=true; check(&in); in.display=false; check(&in);
    }
    /* Independently random palettes, plus mode values outside the normal 0..7 range. */
    for (unsigned n=0;n<262144;n++) {
        in.data=random_word();in.c=random_word();in.color=random_word()&15;
        in.mode=random_word()&((n&1)?255:7);in.display=random_word()&1;in.col=random_word()%40;
        in.cb=random_word();for(int i=0;i<4;i++)in.bg[i]=random_word()&15;check(&in);
    }
    printf("PASS %u character pixel/foreground/fetch/canary cases\n",cases);
}
'''


def function(source, name):
    import re
    match = re.search(r'static (?:inline )?void ' + name + r'\(', source)
    if match is None:
        raise ValueError(f'missing {name}')
    opening = source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def extracted(source):
    return ''.join(function(source, name) for name in ['init_masks', 'mc_masks', 'char_pixels'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/cbm/vic.c')
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='cbm-char-') as temp:
        temp = Path(temp)
        paths = []
        for name, path in [('candidate', args.source)] + ([('reference', args.reference)] if args.reference else []):
            target = temp / (name + '.c')
            target.write_text(HEAD + STATE + extracted(path.read_text()) + TAIL.replace('NAME', name))
            paths.append(str(target))
        test = temp / 'test.c'
        test.write_text(HEAD + TEST)
        exe = temp / 'test'
        cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-O2', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined']
        if args.reference:
            cmd += ['-DWITH_REFERENCE']
        subprocess.run(cmd + paths + [str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
