# VIC multicolor character expansion — 2026-11-08

Follow the [ARE-TYPE profile](aretype-validation-2026-11-08.md) at esp32cbm
`80ae116` / esp32emu `042c86d`. Inclusive `char_pixels` work occupied
4.73–10.71% of core 1, within graphics generation's 5.29–11.82%. These are
nested source shares from a changing game session, not isolated timings.
This experiment keeps memory fetching, hires/invalid-mode semantics,
foreground flags and partial-span handling intact.

## Candidate arithmetic and oracle

Compare three replacements for multicolor pixel composition:

- `muxxor`: choose between colors 0/1 and 2/3 with the low-bit mask, then
  choose the resulting words with the high-bit mask, using XOR selection.
- `muxand`: the same two-level selection with AND/OR masks.
- `pair16`: construct four local 16-bit entries, each containing two copies
  of a palette index. Extract each two-bit selector from the graphics byte,
  look up its pixel pair, and combine four pairs into the 64-bit result.

For `muxxor`, `a = REP(bg[0]); b = REP(c2); a ^= (a ^ REP(c1)) & lo;
b ^= (b ^ REP(c3)) & lo; px = a ^ ((a ^ b) & hi)`.
`muxand` expresses those same selections as `(a & ~mask) | (b & mask)`.
The selected `pair16` expression is in `components/cbm/vic.c`; its table is
an automatic eight-byte array, not persistent RAM. The high-bit mask still
produces the original foreground bytes. The compiler eliminates the unused
low-bit mask work in this branch.

`python3 tools/test_vic_char_pixels.py --reference /path/to/old-vic.c`
extracts actual `init_masks`, `mc_masks` and `char_pixels`. A stub supplies
`g_byte`'s result and checks that it is called exactly once with the requested
column/mode/base; this isolates arithmetic and does not test VRAM mapping.
An independent per-pixel oracle decodes selectors and colors without word
masks, REP multiplication or the local lookup table. Guard words surround
the output pair. ASan/UBSan runs exhaust all 256 data bytes × 256 screen
bytes × 16 colors × eight modes × both display states (16,777,216 cases),
then 262,144 cases with independent random palettes, all 40 columns, base
values and both normal/out-of-range modes. Each candidate and the final
production source pass 17,039,360 cases against the oracle and reference.

## P4 paired kernel benchmark

Tab5: 360 MHz CPU, 200 MHz PSRAM, IDF 5.5.6/GCC 14.2, production cache/XIP
settings and `-O2` options. Use actual extracted `vmem`, `g_byte`, masks and
character/graphics functions. Allocate shared 64 KB RAM and 4 KB character
ROM in PSRAM, with private internal masks/output buffers per implementation.

Compile arithmetic and graphics wrappers in separate translation units so
each has one `char_pixels` caller. The compiler naturally inlines it, as in
the full emulator; no forced-inline attribute is added. An earlier
combined-wrapper harness outlined it and is excluded from the final results.
The initial full board backup completed before this temporary benchmark flash.

Test ten scenes: standard text, multicolor text, mixed hires/multicolor text,
hires bitmap, multicolor bitmap, extended-background text, invalid modes 5/6/7,
and display disabled. Three input sets cover zero/character-ROM data,
all-ones/RAM data and deterministic random RAM data with fine scroll seven.
Each candidate sees identical input. Six kernels exercise forty characters,
a full 400-pixel graphics window, aligned eight pixels, one unaligned pixel,
sixteen pixels, and a border-only span. Foreground and pixel buffers are
compared exactly after warm-up and every ABBA round, including untouched
regions. Each of 540 cases completes seven rounds of 64 calls per batch.
Interrupts remain enabled; guest, display, network and audio tasks are absent.

| Median cycle reduction over multicolor cases | XOR mux | AND/OR mux | Local pair table |
| --- | ---: | ---: | ---: |
| Forty-character arithmetic | 6.48–8.61% | 7.38–11.83% | 18.08–21.04% |
| Full graphics window | 8.70–12.00% | 8.04–11.60% | 15.41–16.77% |
| Eight-pixel span | 7.87–10.23% | 8.18–10.25% | 12.09–13.18% |
| One-pixel span | 5.83–8.09% | 5.14–7.54% | 9.25–10.53% |
| Sixteen-pixel span | 7.27–8.93% | 7.35–8.57% | 11.18–12.30% |

The pair table's mixed-character full windows save 9.77–10.70%. Controls are
not uniformly faster: hires arithmetic ranges from 7.03% less to 1.52% more,
and hires full windows from 1.40% less to 0.13% more. Invalid/display-off
full windows include a 2.59% regression; border-only spans save 0.75–1.41%,
a small wrapper/layout effect rather than evidence of changed border work.
Accept these small control costs for the multicolor savings only if the
full-firmware validation also passes.

Benchmark code/stack: arithmetic wrapper 1,144/128 bytes becomes 962/112;
graphics wrapper 1,622/208 becomes 1,458/176. These are wrapper sizes and
frames, not full-emulator footprint claims. Complete per-case medians and
artifact hashes are in the
[measurement JSON](measurements/char-pixels-2026-11-08.json).

## Firmware and software checks

The seven existing 2,000-frame replay pairs (six VICE sprite-sprite tests and
the generated workload) match all 35 pictures, 14 visible-RAM dumps, seven
PCM captures and CPU/drive status against the retained conversion build.
The separate ARE-TYPE replay also matches the original VIC and retained
conversion builds for 6,000 frames each: eight pictures, three complete
visible-RAM dumps, full PCM, CPU status and state/life/position samples.
Its states include title, gameplay, game over and restarts. This is equivalence
with the preceding emulator, not independent proof of VIC hardware accuracy.

The full linked P4 `draw` shrinks from 2,748 to 2,724 text bytes (−24),
with its 272-byte stack frame unchanged. `commit_line` remains 244 bytes
and stackless; `vic_line_end` remains 2,132 bytes with a 112-byte frame.
Thus the benchmark's stack reduction does not carry through to the larger
inlined production caller. No persistent allocation or global table is added.

Both normal P4 and S3 builds, plus the isolated P4 test build, pass.
Temporary media-folder edits are restored before the normal builds.
Fresh reference/candidate C64 BASIC and generated-workload runs each use two
profiler-free ~15-second frame windows, guest/LCD captures and three complete
32,000-sample profiles per core. The common generated-workload checkpoint
matches all 65,536 CPU-visible bytes and the complete 400×300 guest picture.
The combined frame windows advance 1,507 reference frames/redraws and
1,506 candidate frames/redraws. These are guest-picture counts, not panel FPS.

| Generated workload | Retained conversion reference | Pair-table candidate |
| --- | ---: | ---: |
| Emulated FPS | 50.11–50.15 | 50.11 |
| Core 1 draw | 20.8–23.2% | 20.2–21.7% |
| Core 1 line commit | 14.8–15.3% | 15.0–15.4% |
| Core 1 idle | 34.6–37.6% | 34.6–37.5% |
| Core 0 presenter | 8.3–8.5% | 8.5–8.9% |

These sequential live runs have overlapping idle ranges; they establish no
whole-app headroom or FPS increase. Samples are rounded shares, not
confidence intervals. C64 BASIC remains near 50.13 / 50.10–50.11 FPS, with
75.2–79.0% / 75.6–78.8% core-1 idle and matching static title pixels.

A new candidate ARE-TYPE live session remains near 50.10–50.11 FPS, with
51.7–54.5% core-1 idle, draw at 15.2–19.2%, and presenter at 3.9–4.6%.
Its title and gameplay checkpoints match the preceding conversion build's
full 64 KB visible memory and complete guest pixels. This is an unpaired
live-game control, not evidence for an isolated gain against the preceding
trial's changing game session. Polling records 145/160 samples (90.6%) in PLAY; the remainder are title
or game over. Counts describe the whole session, including inline profiles,
not just the timed windows.
Three extra inline profiles resolve the exact final ELF; inclusive
`char_pixels` shares range 6.08–9.91%, within graphics generation's
6.76–11.11%. These nested values overlap and must not be added or treated as
paired kernel timings against the preceding game's samples.

Candidate C128 40-column and initialized 80-column BASIC boot/pixel controls
match their preceding golden titles (100/45 source rows, removing 80-column
screenshot line repeats). They have no new profiler or timed frame windows.
Initialize 80 columns by resetting with the 40/80 key locked. The first
C128 attempt retained the game's queued typing across the model switch,
adding I/J/K/L characters below READY; it is excluded. Clear queued typing,
release held keys and reset the monitor latch before rerunning both controls.
The clean captures match. This is a harness-state correction, not an emulator
input-behavior change.

All source and whole-LCD captures decode; active core-0 profiles contain the
presenter. All 33 new captures contain 32,000 samples (1,056,000 total).
Retain the pair table for the 15–17% multicolor row-kernel saving and 24 fewer
P4 text bytes, with unchanged draw stack. Accept the documented small kernel
control regressions; no whole-app or S3 runtime improvement is claimed.
Broader games, later stages and physical input/speaker checks remain open.

## Next investigation

Return to the project survey: profile Apple IIgs with active Ensoniq voices
and its 65C816 workload before selecting another optimization. The tested
C64 workloads already meet the PAL cap with substantial headroom; further
local changes need evidence beyond another small synthetic kernel win.

The fresh pre-test 16 MB flash backup is restored with esptool hash
verification, including the previous MSX firmware and NVS settings. Decoded
final guest and whole-LCD images match the preceding MSX Disk BASIC screen
outside the cursor area. No temporary media was uploaded or mounted, and no
benchmark, debugger injection or media-folder edit remains in production
source.
