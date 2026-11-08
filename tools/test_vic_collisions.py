#!/usr/bin/env python3
"""Test actual VIC sprite collision code against a pixel-ownership oracle.

Requires a C compiler with ASan/UBSan. Optionally compare a second vic.c
using --reference PATH, without depending on a particular git history.
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
#define WIN_W 400
#define WIN_X0 (-16)
#define X_WRAP 504
#define IRQ_VIC 1
struct input {
    uint8_t regs[64], active, irq, cpu_irq;
    uint32_t data[8];
    int first, end;
};
struct output { uint8_t regs[64], irq, cpu_irq, calls; };
'''
WRAPPER_HEAD = r'''
static uint8_t r[64], s_sline, s_irq, calls;
static uint32_t s_sdata[8];
static struct { struct { uint8_t irq; } cpu; } cbm;
'''
WRAPPER_TAIL = r'''
void NAME(const struct input *in, struct output *out)
{
    memcpy(r, in->regs, sizeof(r));
    memcpy(s_sdata, in->data, sizeof(s_sdata));
    s_sline = in->active;
    s_irq = in->irq;
    cbm.cpu.irq = in->cpu_irq;
    calls = 0;
    sprite_collisions(in->first, in->end);
    memset(out, 0, sizeof(*out));
    memcpy(out->regs, r, sizeof(r));
    out->irq = s_irq;
    out->cpu_irq = cbm.cpu.irq;
    out->calls = calls;
    assert(!memcmp(s_sdata, in->data, sizeof(s_sdata)));
    assert(s_sline == in->active);
}
'''
TEST = r'''
void candidate(const struct input *, struct output *);
#ifdef WITH_REFERENCE
void reference(const struct input *, struct output *);
#endif
static unsigned cases, split_cases;
static uint32_t seed = 0x8c096741;
static uint32_t random_word(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
/* Build per-pixel ownership sprite-first from individual pattern bits.
 * This deliberately does not use the production per-pixel/slot loop. */
static void oracle(const struct input *in, struct output *out)
{
    uint8_t owners[400] = {0}, colliding = 0;
    memset(out, 0, sizeof(*out));
    memcpy(out->regs, in->regs, 64);
    out->irq = in->irq;
    out->cpu_irq = in->cpu_irq;
    for (int i = 0; i < 8; i++) {
        if (!(in->active & (1u << i))) continue;
        int raw = in->regs[i * 2] + ((in->regs[0x10] & (1u << i)) ? 256 : 0);
        int start = (raw < 504 ? raw : raw - 504) + 16;
        int repeat = (in->regs[0x1d] & (1u << i)) ? 2 : 1;
        bool multi = in->regs[0x1c] & (1u << i);
        for (int bit = 0; bit < 24; bit++) {
            uint32_t mask = multi ? 3u << ((23 - bit) & ~1) : 1u << (23 - bit);
            if (!(in->data[i] & mask)) continue;
            for (int n = 0; n < repeat; n++) {
                int x = start + bit * repeat + n;
                if (x >= in->first && x < in->end) owners[x] |= 1u << i;
            }
        }
    }
    for (int x = in->first; x < in->end; x++) {
        if (owners[x] && (owners[x] & (owners[x] - 1))) colliding |= owners[x];
    }
    if (colliding) {
        if (!out->regs[0x1e]) {
            out->irq |= 4;
            out->calls = 1;
            if (out->irq & out->regs[0x1a] & 15) out->cpu_irq |= 1;
            else out->cpu_irq &= ~1;
        }
        out->regs[0x1e] |= colliding;
    }
}
static void check(const struct input *in)
{
    struct output expected, got;
    assert(in->first >= 0 && in->first <= in->end && in->end <= 400);
    oracle(in, &expected);
    candidate(in, &got);
    if (memcmp(&expected, &got, sizeof(got))) {
        fprintf(stderr, "oracle mismatch at case %u mask %02x clip %d:%d\n",
                cases, in->active, in->first, in->end);
        assert(0);
    }
#ifdef WITH_REFERENCE
    struct output old;
    reference(in, &old);
    assert(!memcmp(&old, &got, sizeof(got)));
#endif
    cases++;
}
static void init(struct input *in)
{
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < 64; i++) in->regs[i] = random_word();
    for (int i = 0; i < 8; i++) in->data[i] = random_word() & 0xffffff;
    in->regs[0x1e] = (random_word() & 1) ? 0 : random_word();
    in->irq = random_word() & 15;
    in->cpu_irq = random_word();
    in->active = random_word();
}
static uint32_t pattern(unsigned n)
{
    switch (n & 7) {
    case 0: return 0;
    case 1: return 0xffffff;
    case 2: return 0xaaaaaa;
    case 3: return 0x555555;
    case 4: return 1u << ((n >> 3) % 24);
    case 5: return 0xffffff ^ (1u << ((n >> 3) % 24));
    case 6: return 0x800001;
    default: return random_word() & 0xffffff;
    }
}
static void position(struct input *in, int i, int raw)
{
    in->regs[i * 2] = raw;
    in->regs[0x10] &= ~(1u << i);
    if (raw & 256) in->regs[0x10] |= 1u << i;
}
static void split_check(const struct input *in)
{
    struct output full, part;
    struct input next = *in;
    candidate(in, &full);
    unsigned calls = 0;
    for (int i = 0; i < 4; i++) {
        next.first = in->first + (in->end - in->first) * i / 4;
        next.end = in->first + (in->end - in->first) * (i + 1) / 4;
        check(&next);
        candidate(&next, &part);
        calls += part.calls;
        memcpy(next.regs, part.regs, 64);
        next.irq = part.irq;
        next.cpu_irq = part.cpu_irq;
    }
    part.calls = calls;
    assert(!memcmp(&full, &part, sizeof(full)));
    split_cases++;
}
int main(void)
{
    struct input in;
    static const int delta[] = {-49,-48,-47,-25,-24,-23,-1,0,1,23,24,25,47,48,49,256,504};
    static const int clips[][2] = {{0,400},{0,0},{400,400},{0,1},{399,400},{8,56},{180,240},{35,36}};
    for (int raw = 0; raw < 512; raw++) {
        for (unsigned d = 0; d < sizeof(delta)/sizeof(*delta); d++) {
            for (int mode = 0; mode < 16; mode++) {
                for (int clip = 0; clip < 4; clip++) {
                    init(&in);
                    in.active = 0x81;
                    position(&in, 0, raw);
                    position(&in, 7, (raw + delta[d]) & 511);
                    in.regs[0x1d] = ((mode & 1) ? 1 : 0) | ((mode & 2) ? 128 : 0);
                    in.regs[0x1c] = ((mode & 4) ? 1 : 0) | ((mode & 8) ? 128 : 0);
                    in.data[0] = pattern(raw + d * 3 + mode + clip);
                    in.data[7] = pattern(raw * 3 + d + mode * 5 + clip);
                    in.first = clip == 0 ? 0 : clip == 1 ? 1 : clip == 2 ? 31 : (raw + d) % 400;
                    in.end = clip == 0 ? 400 : clip == 1 ? 399 : clip == 2 ? 73 : in.first + 1;
                    check(&in);
                }
            }
        }
    }
    static const int edges[] = {0,1,7,8,23,24,31,32,255,256,480,487,488,503,504,505,511};
    for (int mask = 0; mask < 256; mask++) {
        for (int layout = 0; layout < 64; layout++) {
            init(&in);
            in.active = mask;
            for (int i = 0; i < 8; i++) {
                position(&in, i, edges[(layout + i * 3) % 17]);
                in.data[i] = pattern(layout * 7 + i);
            }
            for (int clip = 0; clip < 8; clip++) {
                in.first = clips[clip][0]; in.end = clips[clip][1];
                check(&in);
            }
        }
    }
    for (int n = 0; n < 65536; n++) {
        init(&in);
        in.first = random_word() % 401; in.end = random_word() % 401;
        if (in.first > in.end) { int t = in.first; in.first = in.end; in.end = t; }
        check(&in);
        if (n < 16384) split_check(&in);
    }
    printf("PASS %u pixel/status/IRQ cases and %u split-segment properties\n", cases, split_cases);
}
'''


def function(source, name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='cbm-collisions-') as temp:
        temp = Path(temp)
        inputs = [('candidate', ROOT / 'components/cbm/vic.c')]
        if args.reference:
            inputs.append(('reference', args.reference))
        paths = []
        for name, path in inputs:
            source = path.read_text()
            irq = function(source, 'update_irq').replace('update_irq(', 'actual_update_irq(')
            wrapper = HEAD + WRAPPER_HEAD + irq
            wrapper += 'static void update_irq(void) { calls++; actual_update_irq(); }\n'
            wrapper += function(source, 'sprite_x') + function(source, 'sprite_collisions')
            wrapper += WRAPPER_TAIL.replace('NAME', name)
            target = temp / (name + '.c')
            target.write_text(wrapper)
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
