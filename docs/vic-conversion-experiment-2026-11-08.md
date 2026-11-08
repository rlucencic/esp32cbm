# VIC RGB565 conversion experiment — 2026-11-08

This follows the [collision-span experiment](sprite-collision-experiment-2026-11-08.md)
at esp32cbm `e64a9b6` and esp32emu `f510e53`. The shared production library
is unchanged. The target is `components/cbm/vic.c:commit_line`, which hashes
400 palette indices and converts changed rows to RGB565.

## Attribution and implementation

Three saved 32,000-sample core-1 captures of the retained collision build
attribute 5,974–6,017 samples to line commit. Instruction ranges in the exact
linked ELF put 5,318–5,366 samples in its conversion loop and 569–602 in the
word-FNV loop. Conversion dominates at about 89% of this function's samples;
hashing accounts for about 10%. These are sampled PCs, not isolated phase
cycle measurements. Source-line attribution differs slightly at instruction
boundaries; the measurement JSON records the disassembly ranges and ELF hash.

The old conversion repeatedly derives the output index from the input
pointer. Advance input and output pointers together and write four packed
RGB565 words per eight-pixel iteration. Width is fixed at 400 pixels and the
existing word-alignment and little-endian requirements are preserved. Keep
the palette, FNV hash, unchanged-row suppression, dirty flag and row addressing
unchanged. There is no new table, heap allocation or persistent state.

## Paired P4 kernel measurements

Tab5: ESP32-P4 at 360 MHz, PSRAM at 200 MHz, IDF 5.5.6/GCC 14.2, production
PSRAM instruction/rodata loading and cache settings. Compile actual extracted
functions and palettes in separate translation units with production `-O2`
flags. Calls include the same wrapper assignments and indirect-call overhead.
Interrupts stay enabled; guest, display, network and audio tasks are absent.

Compare pointer loops processing two, four and eight pixels. Each case uses
seven ABBA rounds, 300 calls per batch, after warm-up. Private PSRAM hash
arrays prevent one variant from suppressing another's conversions. Changed
hot rows alternate one valid palette index each call; full-frame batches
alternate that index per pass over all 300 rows. An odd FNV multiplier
preserves this one-word change. Unchanged rows and null output are controls.
Use zero, all-15, ramp and deterministic random indices, an internal hot row,
a PSRAM hot row and a full 240 KB PSRAM frame. All 84 cases complete; dirty
flags and all 300 hashes match between variants after each ABBA round.

| Changed rows: median cycle reduction over four fills | Two pixels | Four pixels | Eight pixels |
| --- | ---: | ---: | ---: |
| Internal hot row | 19.18–19.26% | 26.48% | 30.01–30.03% |
| PSRAM hot row | 19.20–19.23% | 26.44–26.51% | 29.95–30.06% |
| Full PSRAM frame | 18.67% | 26.22–26.36% | 29.65–29.69% |

The eight-pixel variant is selected for full-app validation. PSRAM unchanged
rows cost 0.18–0.19% more in this harness; null-output wrapper calls cost
3.43–3.92% less, a tiny absolute layout/wrapper effect, not a useful fast-path
claim. These timings include hashing and wrapper overhead, rather than
isolating only the conversion instructions. Raw medians are in the
[measurement JSON](measurements/vic-conversion-2026-11-08.json).

The first harness run had PSRAM disabled and failed its allocation assertion
before measurements. It is excluded; the corrected run completes with all
assertions enabled.

The full P4 linked `commit_line` grows from 170 to 244 text bytes (+74) and
remains a stackless leaf. No new RAM is allocated. Both normal production
firmware builds (P4 and S3), plus the isolated P4 test build, pass. Temporary
media namespace changes are restored before the normal builds.

## Correctness

`python3 tools/test_vic_commit_line.py` extracts the actual production
function and palette and runs ASan/UBSan against an independent oracle.
An optional `--reference PATH` also compares the previous implementation.
Each of the three trial loops passes 73,760 cases, and the selected production
source passes another 73,760. Cases cover all 256 color pairs, all 300 rows,
six structured/random fills, matching/different hashes, both initial dirty
states, null output with deliberately invalid row indices, unchanged source
pixels, and guarded hash/output buffers. Matching-hash cases preserve
arbitrary old pixels, including the pre-existing hash-collision behavior.

The existing collision regression passes 819,200 pixel/status/IRQ cases and
16,384 split-segment properties. Seven paired host replays each run 2,000
frames: the original generated workload and the same six VICE sprite-sprite
tests identified in the preceding report. All 35 pictures, 14 complete visible
RAM dumps, seven PCM captures and periodic CPU/drive status match exactly.
This verifies equivalence with the preceding emulator, not independent VIC
hardware accuracy. No external ROMs or test programs are added to the repo.

## Full firmware and board validation

Use the preceding trial's retained collision candidate as the reference:
its source matches `e64a9b6`, with the same shared production code and isolated
`cbmopttest` media folder. Flash each variant with the correct CBM ROMs.
No generated media is uploaded or mounted; inject the original fixture into
temporary guest RAM through the debugger. At the common reset/400-frame/
inject/600-frame checkpoint, all 65,536 CPU-visible bytes and the complete
400×300 guest picture match exactly.

Release execution, then collect two profiler-free ~15-second windows,
guest/LCD captures and three 32,000-sample profiles per core. These are
sequential live workload runs after a common checkpoint, not synchronized
cycle-by-cycle timing or confidence intervals.

| C64 animated workload | Reference | Eight-pixel candidate |
| --- | ---: | ---: |
| Emulated FPS | 50.11–50.13 | 50.12 |
| Core 1 line commit | 18.5–19.3% | 15.1–15.3% |
| Core 1 draw | 21.4–23.2% | 21.6–23.2% |
| Core 1 idle | 30.6–32.2% | 34.4–35.3% |
| Core 0 presenter | 8.2–9.0% | 8.3–8.8% |

Both builds already reach the PAL cap. Retain the loop for the paired kernel
saving and increased full-app headroom, accepting 74 more P4 text bytes.
The full-app line-commit sample reduction is smaller than the isolated 30%
cycle saving: task scheduling, memory contention and sample attribution
remain part of the integrated measurement. No FPS increase is claimed.
C64 BASIC controls remain near 50.06–50.19 / 50.11–50.17 FPS.

C128 40-column and initialized 80-column BASIC both run near 50.11 FPS.
Core-1 idle ranges are 79.9–80.1% and 69.4–73.0%; VDC completion is 6.9%
in 80-column mode. These are candidate controls against the preceding
trial's pixel goldens; no fresh paired C128 timing gain is claimed. Initialize
80 columns by resetting with the 40/80 key locked, rather than selecting an
uninitialized VDC picture after a 40-column boot.

Static title pixels match the fresh C64 reference (first 100 source rows)
and the preceding C128 40-/80-column goldens (100/45 source rows, removing
80-column screenshot line repeats). Cursor-phase regions are excluded.
Both animated runs produce complete guest/LCD captures, and all six active
core-0 profiles include the presenter. All 36 new full-app captures contain
their requested 32,000 samples: 1,152,000 samples in total.

Next validate a real C64 game or obtain S3 runtime measurements before
choosing another low-level optimization. FNV hashing is a much smaller cost
than conversion in this workload. Real-game performance, S3 runtime and
physical speaker/input coverage remain open.

The saved pre-test 16 MB flash image is restored with esptool hash
verification, including the previous MSX firmware and NVS settings. Final
guest and whole-LCD captures show MSX Disk BASIC at `Ok`. No generated media
was uploaded or mounted, and no benchmark, debugger injection or temporary
media-folder change remains in production source.
