#!/usr/bin/env python3
"""Check actual VIC line hashing, conversion and unchanged-row semantics.

Requires a little-endian host and a C compiler with ASan/UBSan.
Optional source/reference inputs allow experiments without editing production.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEAD = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define WIN_W 400
#define WIN_Y0 8
struct input {
    uint8_t pixels[400];
    uint16_t old[400];
    uint32_t hashes[300];
    int row;
    bool dirty, hidden;
};
struct output {
    uint16_t pixels[400];
    uint32_t hashes[300];
    bool dirty;
};
'''
WRAPPER = r'''
static uint8_t s_pix[400] __attribute__((aligned(4)));
static uint16_t *s_out;
static uint32_t *s_hash;
static int s_line;
static bool s_dirty;
'''
TAIL = r'''
void NAME(const struct input *in, struct output *out)
{
    uint32_t guarded_hash[302];
    uint16_t guarded_out[404] __attribute__((aligned(4)));
    guarded_hash[0] = guarded_hash[301] = 0x98765432;
    guarded_out[0] = guarded_out[1] = 0x1357;
    guarded_out[402] = guarded_out[403] = 0x2468;
    memcpy(s_pix, in->pixels, 400);
    memcpy(guarded_out + 2, in->old, sizeof(in->old));
    memcpy(guarded_hash + 1, in->hashes, sizeof(in->hashes));
    s_out = in->hidden ? NULL : guarded_out + 2;
    s_hash = guarded_hash + 1;
    s_line = in->row + 8;
    s_dirty = in->dirty;
    commit_line();
    memset(out, 0, sizeof(*out));
    memcpy(out->pixels, guarded_out + 2, sizeof(out->pixels));
    memcpy(out->hashes, guarded_hash + 1, sizeof(out->hashes));
    out->dirty = s_dirty;
    assert(guarded_hash[0] == 0x98765432 && guarded_hash[301] == 0x98765432);
    assert(guarded_out[0] == 0x1357 && guarded_out[1] == 0x1357);
    assert(guarded_out[402] == 0x2468 && guarded_out[403] == 0x2468);
    assert(!memcmp(s_pix, in->pixels, 400));
}
'''
TEST = r'''
void candidate(const struct input *, struct output *);
#ifdef WITH_REFERENCE
void reference(const struct input *, struct output *);
#endif
static uint32_t seed = 0xa7136c25;
static unsigned cases;
static uint32_t random_word(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static uint32_t hash(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 400; i += 4) {
        uint32_t word = p[i] | ((uint32_t)p[i+1] << 8) |
                        ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24);
        h = (h ^ word) * 16777619u;
    }
    return h;
}
/* Palette specification independent of the production RGB macro/table. */
static const uint8_t palette[16][3] = {
    {0x00,0x00,0x00},{0xff,0xff,0xff},{0x68,0x37,0x2b},{0x70,0xa4,0xb2},
    {0x6f,0x3d,0x86},{0x58,0x8d,0x43},{0x35,0x28,0x79},{0xb8,0xc7,0x6f},
    {0x6f,0x4f,0x25},{0x43,0x39,0x00},{0x9a,0x67,0x59},{0x44,0x44,0x44},
    {0x6c,0x6c,0x6c},{0x9a,0xd2,0x84},{0x6c,0x5e,0xb5},{0x95,0x95,0x95},
};
static uint16_t color(int n)
{
    return ((palette[n][0] / 8) * 2048) |
           ((palette[n][1] / 4) * 32) | (palette[n][2] / 8);
}
static void check(const struct input *in)
{
    struct output expected, got;
    memset(&expected, 0, sizeof(expected));
    memcpy(expected.pixels, in->old, sizeof(expected.pixels));
    memcpy(expected.hashes, in->hashes, sizeof(expected.hashes));
    expected.dirty = in->dirty;
    uint32_t h = hash(in->pixels);
    if (!in->hidden && h != in->hashes[in->row]) {
        expected.hashes[in->row] = h;
        for (int x = 0; x < 400; x++) expected.pixels[x] = color(in->pixels[x]);
        expected.dirty = true;
    }
    candidate(in, &got);
    if (memcmp(&expected, &got, sizeof(got))) {
        fprintf(stderr, "line mismatch case %u row %d hidden %d\n", cases, in->row, in->hidden);
        assert(0);
    }
#ifdef WITH_REFERENCE
    struct output old;
    reference(in, &old);
    assert(!memcmp(&got, &old, sizeof(got)));
#endif
    cases++;
}
static void init(struct input *in)
{
    memset(in, 0, sizeof(*in));
    for (int x = 0; x < 400; x++) {
        in->pixels[x] = random_word() & 15;
        in->old[x] = random_word();
    }
    for (int y = 0; y < 300; y++) in->hashes[y] = random_word();
    in->dirty = random_word() & 1;
}
int main(void)
{
    struct input in;
    for (int a = 0; a < 16; a++) {
        for (int b = 0; b < 16; b++) {
            for (int control = 0; control < 4; control++) {
                init(&in); in.row = (a * 17 + b) % 300; in.dirty = control & 1;
                for (int x = 0; x < 400; x++) in.pixels[x] = (x & 1) ? b : a;
                in.hashes[in.row] = hash(in.pixels) ^ ((control & 2) ? 0 : 1);
                check(&in);
            }
        }
    }
    for (int y = 0; y < 300; y++) {
        for (int fill = 0; fill < 6; fill++) {
            for (int control = 0; control < 4; control++) {
                init(&in); in.row = y; in.dirty = control & 1;
                for (int x = 0; x < 400; x++) {
                    if (fill == 0) in.pixels[x] = 0;
                    else if (fill == 1) in.pixels[x] = 15;
                    else if (fill == 2) in.pixels[x] = x & 15;
                    else if (fill == 3) in.pixels[x] = (x & 1) ? 15 : 0;
                    else if (fill == 4) in.pixels[x] = (x == y) ? 1 : 0;
                }
                in.hashes[y] = hash(in.pixels) ^ ((control & 2) ? 0 : 1);
                check(&in);
            }
        }
    }
    for (int n = 0; n < 65536; n++) {
        init(&in); in.row = random_word() % 300; in.hidden = (n % 8) == 0;
        in.hashes[in.row] = hash(in.pixels) ^ ((n & 1) ? 0 : 1u << (n % 32));
        if (in.hidden) in.row = (n & 8) ? -1000 : 1000000;
        check(&in);
    }
    printf("PASS %u row/palette/hash/dirty/canary cases\n", cases);
}
'''


def function(source):
    start = source.index('static void commit_line(void)')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def palette(source):
    start = source.index('static const uint16_t s_rgb[16]')
    return source[start:source.index('};', start) + 2] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/cbm/vic.c')
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args()
    if sys.byteorder != 'little':
        parser.error('the current VIC word hash requires a little-endian host')
    with tempfile.TemporaryDirectory(prefix='cbm-commit-line-') as temp:
        temp = Path(temp)
        inputs = [('candidate', args.source)]
        if args.reference:
            inputs.append(('reference', args.reference))
        paths = []
        for name, path in inputs:
            source = path.read_text()
            target = temp / (name + '.c')
            target.write_text(HEAD + WRAPPER + palette(source) + function(source) +
                              TAIL.replace('NAME', name))
            paths.append(str(target))
        test = temp / 'test.c'
        test.write_text(HEAD + TEST)
        exe = temp / 'test'
        cmd = shlex.split(os.environ.get('CC', 'cc')) + [
            '-O2', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined']
        if args.reference:
            cmd += ['-DWITH_REFERENCE']
        subprocess.run(cmd + paths + [str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
