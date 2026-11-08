# ARE-TYPE game validation — 2026-11-08

Validate the retained VIC collision and RGB565 conversion optimizations on
an external game, after the generated workload in the
[conversion report](vic-conversion-experiment-2026-11-08.md). This trial adds
replay tooling and evidence; it changes no emulator code.

## Game and builds

Use Jared Evans's MIT-licensed [C64 Horizontal Shoot-'Em-Up (ARE-TYPE)](https://github.com/jaredevans/c64-shmup)
at commit `63554e26e83fb209a41ccf1def1ede4c5f0c4a2e`. Its source includes a
bitmap splash, animated title sprites, character scrolling, raster splits,
sprite multiplexing, enemies, charge/fire controls and three-voice SID music.
This is one homebrew game and a limited scripted run, not general game
compatibility or performance coverage. No game source, art, audio or program
is bundled here.

Assemble externally with ACME `0.97~svn20211115`:

```sh
acme -f cbm -o aretype.prg --symbollist aretype.sym scroll.asm
```

The resulting 30,530-byte PRG has SHA-256
`16e729034364a91894c4e045b7405c04c121135e542dd1b168b3c0ffa9c12a8d`.

Compare three retained firmware/host artifacts with identical shared
production code:

| Variant | CBM source |
| --- | --- |
| Before both VIC changes | `abfd450` |
| After collision spans | `e64a9b6` |
| After row conversion | `8d82639` |

Artifacts were built during the preceding trials, before these changes were
committed; the table identifies their equivalent final source. Measurement
JSON records their hashes. This validation reuses the already tested P4
binaries; no new firmware build or S3 runtime result is claimed.

## Deterministic host replay

`tools/replay_aretype.py` accepts the external PRG/symbol list and the three
host executables. It creates a new output directory and runs each for 6,000
PAL frames, loading through the normal PRG loader at frame 150. Fire presses
include taps and holds/releases; I/J/K/L presses move in a repeating pattern.
Frame-numbered events are identical between variants.

```sh
python3 tools/replay_aretype.py --program /path/to/aretype.prg \
  --symbols /path/to/aretype.sym --reference /path/to/before/cbm_host \
  --collision /path/to/collision/cbm_host \
  --candidate /path/to/conversion/cbm_host --output /tmp/new-aretype-replay
```

All three match eight pictures (frames 350, 650, 900, 1500, 2500, 4000,
5000 and 5999), three complete CPU-visible 64 KB RAM dumps (900, 2500 and
5999), the full PCM capture and periodic CPU/drive status. Game-state,
lives and player-position samples also match exactly. Of 110 state samples
per variant, 12 are title, 93 gameplay and five game over. This includes
restarts and score/life changes; it does not establish boss completion or
late-game accuracy. The repository replay tool reproduces the scratch
harness's results. Host elapsed time is not used as a P4 performance metric.

## Tab5 measurement

Tab5 runs at 360 MHz with 200 MHz PSRAM, IDF 5.5.6/GCC 14.2. Flash each
retained build with the CBM ROMs and select C64. Reset, run 400 frames,
write the unmodified PRG into guest RAM and enter its normal machine-code
entry at `$0810`. No game media is uploaded or mounted. Run 600 frames for
the title checkpoint, queue the same key pattern and run another 300 frames
for gameplay. All three builds match both full 64 KB CPU-visible memory
views and both complete 400×300 guest pictures. Normal PRG loading is
covered separately by the host replays.

Release execution and repeat a queued-key pattern through the debugger every
20 seconds. Poll the game's unmodified state/life bytes every half second.
This input is user-level keyboard typing; no invulnerability, state patches
or other guest-code changes are used. It differs from the host's precise
frame-numbered tap/charge replay. Collect two profiler-free ~15-second frame
windows, guest/LCD pictures, and three 32,000-sample profiles per core.

| Live game session | Before both | Collision only | Collision + conversion |
| --- | ---: | ---: | ---: |
| Emulated FPS | 50.11–50.17 | 50.12–50.17 | 50.11 |
| Core 1 idle | 17.8–39.7% | 47.8–59.3% | 47.8–51.9% |
| Core 1 draw | 35.0–51.1% | 13.1–20.3% | 17.9–21.4% |
| Core 1 line commit | 3.5–5.8% | 4.0–6.1% | 4.5–5.2% |
| Core 1 6502 execution | 9.8–11.2% | 10.9–11.8% | 11.2–11.7% |
| Core 0 presenter | 3.8–4.3% | 3.7–4.3% | 4.1–4.5% |

The frame windows advance 1,507 / 1,507 / 1,506 guest frames and 1,444 /
1,408 / 1,443 redraws. These are guest-picture counters, not panel FPS.
The source and whole-LCD captures decode correctly and show the game;
all active core-0 profiles include the presenter.

State polling records 130/139, 110/120 and 167/182 samples in PLAY
(93.5%, 91.7%, 91.8%). The other samples are title/game over, rather than
an idle BASIC screen. Final-build polling includes the extra inline profiles
below and therefore covers a longer interval. Counts describe the whole
capture session, not a label for each profiler sample. Live inputs, deaths,
respawns and scene complexity can differ with wall-clock timing after the
common checkpoint. The wide sample ranges reflect this changing workload.
These sequential sessions are not paired phase timings or confidence
intervals; do not divide their endpoints to derive a precise kernel speedup.

The observed draw/headroom ranges support the collision optimization in this
real game. All builds already meet the PAL cap, so no FPS increase is claimed.
The collision-only and final ranges overlap: this run does **not** isolate an
additional full-game benefit from the conversion loop. Its retained evidence
remains the preceding paired-kernel and generated-workload measurements.

## Next target

Three additional final-build core-1 captures resolve every PC through DWARF
inline frames. Inclusive graphics generation occupies 5.29–11.82%, including
`char_pixels` at 4.73–10.71%. Collision scanning is 1.78–6.68%, sprite
composition 0.46–1.07%, line commit 2.40–4.61%, and 6502 execution
10.24–11.63%. These nested shares overlap and must not be added. The new
21 captures all contain 32,000 samples (672,000 samples total).

The next bounded C64 experiment is `char_pixels`, especially multicolor
mask/palette expansion: separate its arithmetic from `g_byte`/VRAM access,
compare a portable C rewrite with the current implementation, and measure
code/stack costs and representative partial spans before considering assembly.
Keep the current implementation unless a candidate passes the native oracle,
this game replay and paired P4 kernel measurements. Broader games, later
stages, S3 runtime and physical speaker/input verification remain open.

The saved pre-test 16 MB image is restored with esptool hash verification,
including the previous MSX firmware and NVS settings. Decoded final guest and
whole-LCD pictures match the preceding restored MSX Disk BASIC screen except
for cursor blink. No test media was uploaded or mounted, and no production
source was changed.
