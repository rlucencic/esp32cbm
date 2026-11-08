#!/usr/bin/env python3
"""Generate an original C64 VIC/SID profiling PRG (not a correctness oracle)."""
import argparse
from pathlib import Path
import struct


def fixture():
    # BASIC line 10 SYS 2064, then machine code at $0810.
    image = bytearray(bytes.fromhex('0b080a009e32303634000000'))
    image.extend(bytes(0x810 - 0x801 - len(image)))
    code = bytearray()
    labels, fixups = {}, []

    def emit(*values):
        code.extend(values)

    def store(address, value):
        emit(0xa9, value, 0x8d, address & 255, address >> 8)

    def label(name):
        labels[name] = len(code)

    def branch(opcode, name):
        emit(opcode, 0)
        fixups.append((len(code) - 1, name))

    emit(0x78)  # SEI: no ROM IRQ code competes with the fixture.
    store(0xdc0d, 0x7f)
    store(0xdd0d, 0x7f)
    store(0x0001, 0x35)  # RAM with I/O visible.
    store(0xd011, 0x1b)
    store(0xd016, 0x18)  # Multicolor character mode.
    store(0xd018, 0x14)
    for address, value in [(0xd020, 0), (0xd021, 6), (0xd022, 5),
                           (0xd023, 3), (0xd025, 5), (0xd026, 10),
                           (0xd015, 255), (0xd010, 128), (0xd01c, 170),
                           (0xd01d, 85), (0xd017, 170), (0xd01b, 85)]:
        store(address, value)
    for i in range(8):
        store(0x07f8 + i, 0x80 + i)
        store(0xd000 + i * 2, (40 + i * 32) & 255)
        store(0xd001 + i * 2, 70 + i * 17)
        store(0xd027 + i, i + 1)
    emit(0xa2, 0)
    label('init')
    emit(0x8a, 0x49, 0xa5)
    for address in [0x2000, 0x2100]:
        emit(0x9d, address & 255, address >> 8)
    emit(0x8a, 0x29, 15)
    for address in [0xd800, 0xd900, 0xda00, 0xdb00]:
        emit(0x9d, address & 255, address >> 8)
    emit(0xe8)
    branch(0xd0, 'init')
    # Three gated voices: saw, triangle and pulse, routed into low-pass.
    for i, (frequency, control) in enumerate([(0x1000, 0x21),
                                             (0x1800, 0x11),
                                             (0x2400, 0x41)]):
        base = 0xd400 + i * 7
        for offset, value in [(0, frequency & 255), (1, frequency >> 8),
                              (2, 0), (3, 8), (5, 0x11), (6, 0xf1),
                              (4, control)]:
            store(base + offset, value)
    for address, value in [(0xd415, 0), (0xd416, 0x40),
                           (0xd417, 0x77), (0xd418, 0x12), (0x0002, 0)]:
        store(address, value)
    label('frame')
    # Wait for raster zero, update all 1000 visible character codes and
    # move every sprite horizontally. No raster IRQ or disk activity.
    label('wait_nonzero')
    emit(0xad, 0x12, 0xd0)
    branch(0xf0, 'wait_nonzero')
    label('wait_zero')
    emit(0xad, 0x12, 0xd0)
    branch(0xd0, 'wait_zero')
    emit(0xe6, 2, 0xa2, 0)
    label('chars')
    emit(0x8a, 0x18, 0x65, 2)
    # Stop before the sprite-pointer bytes at $07f8.
    for address in [0x0400, 0x0500, 0x0600]:
        emit(0x9d, address & 255, address >> 8)
    emit(0xe0, 0xe8)
    branch(0xb0, 'skip_tail')
    emit(0x9d, 0, 7)
    label('skip_tail')
    emit(0xe8)
    branch(0xd0, 'chars')
    for i in range(8):
        emit(0xee, i * 2, 0xd0)  # INC sprite X low byte.
    address = 0x810 + labels['frame']
    emit(0x4c, address & 255, address >> 8)
    for position, name in fixups:
        distance = labels[name] - position - 1
        assert -128 <= distance <= 127
        code[position] = distance & 255
    return struct.pack('<H', 0x801) + image + code


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_bytes(fixture())
    print(f'Wrote {len(fixture())} bytes to {args.output}')
