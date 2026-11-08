#!/usr/bin/env python3
"""Deterministic ARE-TYPE replay; see docs/aretype-validation-2026-11-08.md.

Build the game and symbol list externally; no game assets are bundled.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

def main():
    parser = argparse.ArgumentParser(description='Replay the pinned ARE-TYPE game through three CBM host builds; compare pictures, RAM, PCM and status.')
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--symbols', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--collision', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new directory for replay artifacts')
    args = parser.parse_args()
    r = args.output.resolve()
    r.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    symbols = {n: int(a, 16) for n, a in re.findall('^\\s*(\\w+)\\s*= \\$([0-9a-fA-F]+)', args.symbols.read_text(), re.M)}
    variants = {'before-both': args.reference.resolve(), 'after-collision': args.collision.resolve(), 'after-conversion': args.candidate.resolve()}
    shots = [350, 650, 900, 1500, 2500, 4000, 5000, 5999]
    ram = [900, 2500, 5999]
    for variant, exe in variants.items():
        d = r / 'host' / variant
        d.mkdir(parents=True, exist_ok=True)
        lines = []
        for f in shots:
            lines.append((f, f'shot {d}/{f}.ppm'))
        for f in ram:
            lines.append((f, f'dumpram {d}/{f}.bin'))
        for f in range(500, 6000, 50):
            for n in ['gameState', 'lives', 'player_x', 'player_y']:
                lines.append((f, f'peek {symbols[n]:x}'))
        for f in range(800, 5900, 120):
            lines.extend([(f, 'key 60 1'), (f + 10, 'key 60 0'), (f + 40, 'key 60 1'), (f + 100, 'key 60 0')])
        for f in range(1000, 5800, 240):
            lines.extend([(f, 'key 33 1'), (f + 60, 'key 33 0'), (f + 60, 'key 42 1'), (f + 100, 'key 42 0'), (f + 120, 'key 37 1'), (f + 180, 'key 37 0'), (f + 180, 'key 34 1'), (f + 220, 'key 34 0')])
        script = d / 'script'
        script.write_text(''.join((f'{f} {cmd}\n' for f, cmd in sorted(lines, key=lambda a: a[0]))))
        env = dict(os.environ, CBM_ROMS=str(repo / 'roms'), CBM_PRG=str(args.program.resolve()), CBM_SCRIPT=str(script), CBM_WAV=str(d / 'audio.wav'), CBM_STATUS='100')
        with (d / 'run.log').open('w') as log:
            subprocess.run([exe, 'c64', '6000'], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        print('RAN', variant, flush=True)
    base = r / 'host/before-both'
    result = {'frames_each': 6000, 'variants': {}, 'state_address': hex(symbols['gameState']), 'program_sha256': hashlib.sha256(args.program.read_bytes()).hexdigest()}
    for variant in variants:
        d = r / 'host' / variant
        hashes = {}
        for n in [*(f'{f}.ppm' for f in shots), *(f'{f}.bin' for f in ram), 'audio.wav']:
            a, b = ((base / n).read_bytes(), (d / n).read_bytes())
            assert a == b, (variant, n)
            hashes[n] = hashlib.sha256(b).hexdigest()

        def status(p):
            return [l for l in p.read_text().splitlines() if l.startswith(('frame ', 'host: frame')) or 'instructions per frame:' in l]
        assert status(base / 'run.log') == status(d / 'run.log'), variant
        states = re.findall('\\$' + f'{symbols['gameState']:04X}' + ' = \\$([0-9A-F]+)', (d / 'run.log').read_text())
        counts = {str(i): states.count(f'{i:02X}') for i in range(3)}
        assert counts['1'] > 0, counts
        result['variants'][variant] = {'hashes': hashes, 'state_samples': counts}
        print('MATCH', variant, counts, flush=True)
    (r / 'host-results.json').write_text(json.dumps(result, indent=2))
if __name__ == '__main__':
    main()
