# VIC sprite collision spans — 2026-11-08

## Reference and candidate

Reference: esp32cbm `abfd450`; shared production code is unchanged from
esp32emu `288b73b` (later shared commits change documentation only).
The [baseline](profile-baseline-2026-11-08.md) attributes 44.88–45.03% of
core 1 to sprite-sprite collision scanning, including its inlined helpers,
in the generated animated-character/eight-sprite/three-voice workload.
That is a synthetic bottleneck, not an ordinary-game profile.

The original `sprite_collisions` loops over every pixel in a drawing
segment and tests all eight sprite slots. For each active slot it computes
X wrapping, width and multicolor/expansion flags again. The candidate
collects visible sprite descriptors once, clips their span union to the
requested segment, and scans only that union and the collected slots.
Fewer than two visible spans cannot collide and return immediately.
Transparent pattern bits remain part of the conservative span.

The opacity calculation and final collision-latch/IRQ update are unchanged.
There is no new persistent state, heap allocation, table or assembly.
Descriptors occupy the stack. In the linked P4 app, VIC `draw` grows from
2578 to 2748 text bytes (+170) and its compiler stack frame from 192 to
272 bytes (+80). `vic_line_end` remains 2132 bytes with a 112-byte frame;
`commit_line` remains a 170-byte leaf. Collision work remains inlined
into `draw`, so a flat symbol profile must include the new inline body.
The emulation task has an 8192-byte stack; no stack-size setting changes.

Both the isolated benchmark and full apps use ESP-IDF 5.5.6 / GCC 14.2,
360 MHz P4 and 200 MHz PSRAM, with the component's normal `-O2` flags.
S3 and P4 normal builds pass; S3 runtime is unavailable. No shared
production C changes, so this trial does not require a new sibling matrix.

## Native correctness

`python3 tools/test_vic_collisions.py` compiles the actual production
collision function, X-wrap helper and IRQ update under ASan/UBSan. A
sprite-first oracle expands individual monochrome bits or multicolor
pairs into a pixel-ownership map, then derives collision/IRQ state.
It does not reuse the production pixel-first/slot loop.

The retained regression runs 819,200 pixel/status/IRQ cases:

- 557,056 cases cover all 512 raw X positions, seventeen relative offsets,
  sixteen two-sprite expansion/multicolor combinations and four clips.
  Pattern selection cycles through zero, full, alternating, single-bit,
  inverse single-bit, edge-bit and deterministic random data.
- 131,072 cases cover all 256 active masks, 64 edge/wrap layouts and eight
  clips, including empty, one-pixel and full-width intervals.
- 65,536 cases randomize active masks, positions, modes, patterns, clips,
  collision latches, VIC flags/masks and other CPU IRQ bits.
- 65,536 further checks divide 16,384 random calls into four consecutive
  segments. Their combined state and IRQ-update count must match one call.

All 64 VIC registers, VIC interrupt flags, CPU IRQ bits and the number of
IRQ updates match the oracle. Input pattern data and active masks remain
unchanged. Empty/nonempty collision latches and unrelated interrupt bits
are checked. Priority, colors and border flags are randomized but do not
suppress sprite-sprite opacity. ASan/UBSan checks bounds and shifts.

This run also compares the actual original source using
`--reference /path/to/original/vic.c`: every case matches. The optional
reference input avoids binding the regression tool to a particular git
history. These are emulator-semantics checks; existing hardware timing
limitations are not corrected or independently certified by this trial.

## Host software replays

The six sprite-sprite PRGs from the
[VICE testprogs mirror](https://github.com/libsidplayfp/VICE-testprogs/tree/e7aa2aab2e5ac4208fcd53acd4bb237902149e4f/VICII/spritecollisions)
cover the four hires/multicolor combinations, the general collision
program and the cycle-oriented program. The generated baseline workload
is a seventh replay. Each original/candidate pair runs 2000 C64 frames
using the local ROMs and automatic PRG loading at frame 150.

All 35 picture checkpoints (250, 500, 1000, 1500, 1999), fourteen complete
64 KB RAM snapshots, seven supported-audio WAVs and periodic CPU/drive
status logs match exactly. Test programs and ROMs remain outside git.
This establishes no regression against the existing emulator, not a pass
against each test's real-C64 timing expectation. No later-stage game,
physical joystick or speaker-fidelity coverage is claimed.

## Isolated P4 benchmark

A minimal app pins the benchmark task to core 1. Reference and candidate
wrappers contain the actual source functions and internal-RAM state.
Each timed batch calls collision scanning 256 times, with interrupts
still enabled. State setup/copies and status-check assertions occur
outside timing. Collisions latch during warmup, as in the generated
workload; correctness tests cover the IRQ transitions separately.

Seven ABBA rounds per case alternate original/candidate/candidate/original.
Report the median paired cycles per call and paired relative change.
The timed call includes the wrapper/call overhead; no guest CPU, LCD,
network or speaker runs. This is one firmware run with repeated pairs,
not confidence intervals or a whole-emulator speedup. The final checksum
is 93191.

| Workload | Reference cycles | Candidate cycles | Change |
| --- | ---: | ---: | ---: |
| No active sprites | 11.60 | 11.07 | -4.70% |
| One active sprite | 16.07 | 14.08 | -12.50% |
| Two overlapping / full segment | 42696.49 | 2004.28 | -95.31% |
| Two far apart / full segment | 43073.12 | 10322.13 | -76.04% |
| Eight spread / normal width | 74022.77 | 38330.76 | -48.22% |
| Eight overlapping / normal width | 77254.06 | 5969.31 | -92.27% |
| Eight spread / X-expanded | 76831.73 | 43669.59 | -43.16% |
| Two fully offscreen | 34973.03 | 148.87 | -99.57% |
| One visible and one offscreen | 41687.11 | 163.12 | -99.61% |
| X wrap / only one visible | 40606.86 | 172.64 | -99.57% |
| Two multicolor / sparse bits | 42851.43 | 1709.63 | -96.01% |
| Two zero patterns | 41437.71 | 1541.33 | -96.28% |
| Two sprites / 8-pixel partial | 1041.68 | 655.12 | -37.10% |
| Two overlapping / 1-pixel control | 177.77 | 259.60 | +46.03% |
| Eight active / empty control | 18.85 | 243.59 | +1191.99% |
| Eight outside 20-pixel clip | 4039.26 | 245.00 | -93.93% |
| Eight spread / mixed modes | 76439.61 | 32763.66 | -57.14% |
| Two overlapping / 4-pixel partial | 563.27 | 457.04 | -18.86% |
| Eight overlapping / 4-pixel partial | 1245.47 | 1343.17 | +7.84% |
| Eight overlapping / 8-pixel partial | 2447.26 | 2267.10 | -7.36% |
| Eight overlapping / 16-pixel partial | 4847.17 | 4119.12 | -15.02% |
| Two overlapping / 2-pixel control | 309.23 | 324.22 | +4.85% |

Full-width overlapping/spread cases cost 43.16–96.28% less; clipping away
all but one sprite avoids almost all of the old scan. Small calls expose
the setup cost: eight overlapping sprites over four pixels regress 7.84%,
while the eight-pixel equivalent saves 7.36%. One-/two-pixel controls also
regress, and an empty call spends 244 cycles preparing descriptors.
The current `draw` caller rejects empty segments; raster-derived interior
boundaries are eight pixels apart, with four-pixel window-edge segments.
The dense four-pixel tradeoff therefore remains relevant. The small
no-/one-active differences are layout effects in unchanged early returns.
Do not use these tiny differences as evidence of an improved algorithm.

## Full firmware validation and decision

Use the exact saved baseline test app and a freshly compiled candidate,
both with isolated `cbmopttest` media settings and the same shared runtime.
Verify a real `vic.c` compile and the different linked `draw` sizes to
exclude stale objects. Normal source is restored to `cbmdisks` before its
P4/S3 builds. The [numeric summary](measurements/sprite-collision-2026-11-08.json)
records both ELF/app SHA-256 values and all measurement ranges.

For C64, first measure idle BASIC, then reset/stop through the debugger,
advance 400 relative guest frames, inject the generated PRG body at $0801,
set PC to its $0810 machine-code entry, and advance 600 more relative
frames. Absolute debugger frame counters need not match. At that paused
checkpoint, the guest picture and all sixteen 4 KB CPU-visible memory
blocks match exactly. This board entry bypasses BASIC's SYS loader;
normal PRG loading is covered by the host replays.

Release execution, then collect two profiler-free ~15-second windows,
guest/LCD captures and three 32,000-sample profiles on each core. Both
whole-LCD pictures decode to the expected complete animated scene; all
six active-workload core-0 captures include the presenter. These are
live repeated workloads after a common checkpoint, not synchronized
instruction-by-instruction timing runs.

| C64 active workload | Reference | Candidate |
| --- | ---: | ---: |
| Emulated FPS, two windows | 48.71–48.75 | 50.05–50.17 |
| Core 1 `draw` share | 60.2–60.6% | 21.3–23.1% |
| Core 1 `commit_line` share | 16.1–16.5% | 18.8–18.9% |
| Core 1 `draw` + `commit_line` | 76.4–76.7% | 40.1–42.0% |
| Core 1 idle | absent from top-40 entries | 31.2–32.0% |
| Core 0 presenter | 8.2–8.7% | 8.3–8.6% |
| Core 0 idle | 58.8–60.8% | 59.8–60.6% |

The reference advances 1465 guest frames/redraws in its window; the
candidate advances 1507. Pooled frame rate rises about 2.8% to the PAL
real-time cap, with substantial core-1 headroom. Redraw counts describe
guest pictures, not panel FPS. Percentages are sample shares with rounded
symbol resolution, not confidence intervals. `commit_line`'s code is
unchanged even though its integrated share rises; these runs change the
throughput and memory-work schedule, so that share change does not isolate
conversion performance.

BASIC is a separate control: fresh reference/candidate C64 windows stay
near 50.11 FPS. Core-1 idle ranges are 76.8–78.7% / 74.8–78.8%, with no
consistent headroom improvement claimed there. Candidate C128 40-column
and initialized 80-column BASIC stay at 50.11–50.12 and 50.11–50.17 FPS.
Their core-1 idle ranges are 79.1–79.3% and 71.6–72.0%; VDC frame completion
is 6.8–7.0% in 80-column mode. These C128 checks use the preceding baseline's
pixel goldens; no fresh original C128 timing pair is claimed.

Static title RGB pixels match the fresh reference in C64's first 100 source
rows and the preceding goldens in C128 40-column's first 100 and 80-column's
first 45 source rows (after removing screenshot line repeats). Cursor-phase
regions are excluded. Select 80 columns, then debugger-reset with the
40/80 key locked down to initialize BASIC there; simply selecting the
other monitor after a 40-column boot is not a valid text control.

Three additional candidate core-1 captures resolve every sampled PC through
DWARF inline frames. Collision scanning, including its nested helper work,
now occupies 5.49–7.01%; `draw_graphics` is 12.95–13.43% and `commit_line`
18.67–18.80%. These inclusive frames overlap; do not add nested rows or
compare them as a paired kernel timing against the earlier baseline.
**Next profile `commit_line`'s hash and RGB565 conversion separately.**

Retain the span-limited C implementation for the large paired-kernel
savings and the measured synthetic full-app headroom, accepting +170 text
bytes, +80 common draw-stack bytes and the dense four-pixel setup cost.
All 39 new full-app/inline captures contain their requested 32,000 samples
(1,248,000 samples total). The production-only native regression also
passes after all temporary source changes are restored. S3 runtime,
real-game performance and later game stages remain open.

After testing, no generated media is mounted on the SD card: the fixture
was injected through the debugger into temporary guest RAM. The saved
pre-test 16 MB flash image is restored with esptool hash verification,
including the previous NVS settings. Final guest and decoded whole-LCD
captures show MSX2 Disk BASIC at `Ok`, running near 60 FPS. No benchmark,
debugger-injection code or isolated media namespace remains in production
source. The retained change is only in VIC collision scanning.
