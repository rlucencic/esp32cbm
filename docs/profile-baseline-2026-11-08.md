# Commodore profiling baseline — 2026-11-08

## Purpose and reference

Continue the emulator-by-emulator optimization survey after MSX. The static
assessment names the shared 6502 core, VIC character/sprite work and SID
synthesis; measure their contribution before selecting a rewrite.
This is a baseline investigation, with no production emulation change.
Commodore source reference: `b9fdb12`.
Shared runtime reference: `288b73b`, including the Tab5 flush and presenter
snapshot fixes. The attached board is a Tab5/P4; S3 runtime is unavailable.

## Generated workload

`python3 tools/profile_fixture.py /tmp/opt-cbm-profile.prg` generates an
original 491-byte C64 BASIC `SYS 2064` program plus machine code. It disables
CIA interrupts, selects RAM with I/O visible, and configures multicolor
characters, eight sprites with mixed multicolor/priority/X/Y-expansion
masks, and three gated SID voices routed through the low-pass filter.
The volume nibble is two. Sprite data and pointers are deterministic.
Sprite Y starts are 70, 87, …, 189; mixed Y expansion gives a varying
number of simultaneously active sprites on each line.

After a raster-zero wait it changes all 1000 visible character codes and
increments each sprite's X low byte. High-X bits remain fixed, so wrapping
is intentional. It does not implement a game, exercise disk loading, or
validate real VIC/SID timing. Use it to expose active graphics/audio costs,
with idle BASIC as a separate control; do not treat it as a representative
game or an independent hardware correctness oracle.

The generator is the retained workload source. Example host run:

```sh
make -C components/cbm/host
python3 tools/profile_fixture.py /tmp/opt-cbm-profile.prg
CBM_ROMS="$PWD/roms" CBM_PRG=/tmp/opt-cbm-profile.prg \
  CBM_WAV=/tmp/cbm-profile.wav \
  components/cbm/host/build/cbm_host c64 1000 /tmp/cbm-profile.ppm
```

Two host runs match exactly at three picture checkpoints (frames 500, 750
and 999), a complete 64 KB RAM dump and the supported SID PCM capture.
The pictures change between checkpoints, and the sprite pointers/pattern
bytes match the generator. PCM spans -4280 through 4366 over 614,306 stereo
sample frames, confirming nonconstant output without assessing fidelity.
C64 BASIC and C128 BASIC boot checks also pass on the host. Host timings
are not ESP32 performance measurements.

## Board method

Back up the previous full 16 MB flash before switching machines. Flash the
Commodore bootloader, partition table, app and local ROM images without
erasing NVS. Test firmware uses isolated `cbmopttest` media settings;
production source retains `cbmdisks`. Upload only the generated temporary
PRG, use Program slot 4, and remove it after measurement. Existing images
are not mounted or modified by this test build.

Measure C64 BASIC, the generated active workload, C128 40-column BASIC and
C128 80-column BASIC separately. For each, collect two profiler-free
~15-second frame/redraw windows, guest and whole-LCD pictures, then three
32,000-sample captures on each core (nominal 20 kHz) against the exact test
ELF. CPU/PSRAM clocks are 360/200 MHz, ESP-IDF 5.5.6 / GCC 14.2,
with the normal component optimization flags. The speaker task runs on core 0,
so core-1-only profiles would miss SID synthesis. VIC signatures are incrementing redraw counters; the
80-column signature encodes the VDC counter as `counter * 2654435761 + 1`
modulo 2³². Decode it with the modular inverse before counting redraws.
Neither measure is panel FPS.
Short repeated captures are descriptive ranges, not confidence intervals.

## C64 result and selected target

| Metric | BASIC | Generated VIC/SID workload |
| --- | ---: | ---: |
| Emulated FPS, two profiler-free windows | 50.11–50.18 | 48.77–48.85 |
| Core 1 `m6502_run` symbol share | 8.4–8.7% | 9.8–10.1% |
| Core 1 `draw` symbol share | 1.0–1.2% | 59.7–60.3% |
| Core 1 `commit_line` symbol share | 0.2% | 15.9–16.3% |
| Core 0 presenter symbol share | 0.4% | 8.2–8.4% |
| Core 0 idle | 86.2–86.3% | 59.5–61.3% |

The generated workload advances 1468 redraw signatures over its ~30-second
window; BASIC advances 90. Active LCD-on captures include presenter work
in all three core-0 profiles. The drives remain idle. Sound is rendered,
but physical speaker quality is not assessed.

Because GCC inlines several VIC helpers into `draw`, take three additional
32,000-PC core-1 captures and resolve each sampled address with the linked
ELF's DWARF inline call chain (`addr2line -a -f -i`). Count each sample once
per named frame in that chain. The following are inclusive source-frame
shares: nested rows overlap and must not be added together.

| Inline frame | Core 1 share |
| --- | ---: |
| `draw` | 59.60–60.38% |
| `sprite_collisions`, including inlined `sprite_x` | 44.88–45.03% |
| `draw_graphics`, including character conversion | 11.72–12.32% |
| `char_pixels`, nested in graphics | 10.42–10.95% |
| `draw_sprites` | 2.38–2.63% |
| `commit_line` | 16.19–16.50% |

These captures include all requested samples. An earlier immediate second
request produced an incomplete response and is excluded; allow the serial
console to finish and drain its response between requests. Only complete
captures contribute to the ranges.

Select **VIC sprite-sprite collision scanning** as the next bounded
experiment. The current loop visits every segment pixel and tests eight
sprite slots, repeatedly calculating each sprite's position and mode.
First try clipping work to active sprite spans and moving invariant decode
out of the pixel loop. Compare that portable C approach before assembly.
Keep the current implementation as the reference.

The correctness boundary includes 504-position X wrapping, all clip
intervals and partial raster-line calls, multicolor nonzero-pair opacity,
X expansion, every active mask, zero/all/random pattern bits, multiple
simultaneous collisions and preservation of latched `$D01E`/IRQ state.
Sprite priority and border visibility do not suppress sprite-sprite
collisions. Test those semantics independently, then replay real software
before claiming broader benefit. The generated case intentionally stresses
sprite collisions and does not establish their share in ordinary games.

## C128 controls and excluded setup

The first 80-column selection only toggles the monitor key after booting
40-column BASIC. Its guest and LCD captures decode to all black, as does
the same host sequence: selecting the other monitor does not initialize
its BASIC screen. Do not call that a successful 80-column BASIC run or
use its profile as a text-rendering baseline. A host repeat resets with
the 40/80 key locked down and produces the expected nonblack VDC picture;
the board control is repeated with that same sequence.

HTTP status requests occasionally fail after serial captures have finished.
Those failures do not truncate the saved serial profiles or frame windows.
The repeat uses the attached board's address from its boot log and bounded
HTTP retries; no network address or credentials are included in this report.

The corrected board capture contains BASIC 7.0's title and READY prompt in
both the guest picture and the decoded LCD picture. Its first 800×45 source
pixels have exactly the same nonblack glyph mask as the corrected host
capture, after removing their different screenshot line repeats. This
avoids RGB565-to-RGB888 rounding and cursor-phase differences.

The corrected C128 controls give these ranges:

| Metric | 40-column BASIC | 80-column BASIC after reset |
| --- | ---: | ---: |
| Emulated FPS | 50.05–50.17 | 50.12 in both windows |
| Core 1 interpreter | 8.7–8.9% | 8.8–9.1% |
| Core 1 VDC frame completion | not shown | 6.8–6.9% |
| Core 1 idle | 76.3–79.9% | 69.2–72.7% |
| Core 0 presenter | 0.3–0.4% | 1.0–1.1% |
| Core 0 idle | 85.5–86.2% | 85.0–85.5% |
| Guest redraws during ~30 seconds | 76 | 188 |

All four final cases have complete profiles: 24 flat-symbol captures plus
three additional C64 inline-resolved captures, 864,000 samples total.
The [measurement summary](measurements/profile-baseline-2026-11-08.json)
retains ranges, artifact hashes and the separate inline captures without
private network data or ROM/media contents. The normal P4 app rebuild
passes and its source uses the original media namespace. Only the test
fixture generator and documentation are new; production C remains unchanged.
The prior shared-runtime S3/P4 matrix includes this Commodore source, but
there is no new S3 runtime or performance result from this investigation.

The generated PRG is ejected and deleted, and the saved pre-test 16 MB
flash image is restored with esptool's hash verification. Final guest and
whole-LCD captures show the previous MSX2 Disk BASIC at `Ok`, with frames
continuing near 60 FPS. This also restores the previous NVS settings;
original Commodore media settings are not opened for a normal test run.
The shared assessment now selects the collision-scan experiment. The baseline adds a report, numeric summary and reproducible generator.
