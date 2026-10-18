# esp32cbm: Commodore 64 / 128 on the Makerfabs MaTouch ESP32-S3 7"

A Commodore 64 and a Commodore 128 (PAL): 6510/8502 every bus cycle, VIC-II,
two 6526s, SID; the C128's MMU, VDC (80 columns), FAST (2 MHz) and Z80
(start-up, CP/M 3.0). Drives 8 and 9 emulated whole (1541-II on the C64,
1571 on the C128: their own 6502, VIAs, GCR; the 1571's CIA for fast serial).
Disks D64/D71/G64, cartridges CRT, programs PRG/T64, an LVGL setup screen,
a web page for images and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console), the 6502 core (`components/m6502`) and the Z80 core
(`components/z80`, with `CONFIG_Z80_READ_SPECIAL` here) are the shared
library in `../esp32emu`, also used by `../esp32ste`, `../esp32mac`,
`../esp32cpc`, `../esp32zx`, `../esp32xl` (which shares the 6502 core),
`../esp32msx` and `../esp32gs`. See its CLAUDE.md for the runtime and the
board-level lessons; changes there need all eight projects built, for both
boards.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac.

## Build, flash, debug

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 -b 115200 app-flash'
```

- Flash with `-b 115200` (460800 failed while writing, as on the ZX).
- `app-flash` writes only the app; `idf.py flash` also writes the ROMs in
  `roms/` (not in git) into 16 KB slots of the `cbm_roms` partition
  (0x810000, 256 KB): c64basic 0, c64kernal 1, c64chargen 2, 1541 3,
  c128basic 4-5, c128kernal 6, c128chargen 7, c128c64 8, 1571 9-10 (top
  `CMakeLists.txt`, `main/rom_store.c`). The partition overlaps the other
  machines' ROM partitions: switching the board needs a full `idf.py flash`
  there (and here). NVS (WiFi, BLE pairings) is shared.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset | profile ELF [N]`
  with the IDF venv's Python (`-I`). One touch per `send`. `send` ends each
  argument with a newline, so a `\n` at the end of a `T` line doesn't reach
  the machine: send RETURN as `k 1`.
- `profile build/esp32cbm.elf` samples core 1 at 20 kHz (the runtime's `p`
  command) and prints the hottest functions: the tool that found most of
  the speed below.
- `curl http://commodore.local/api/status`; disks: see `../esp32emu/README.md`
  (drives 1 8:, 2 9:, 3 Cartridge, 4 Program).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i` `p`, see
`emu/console.h`; `F <d> +` makes the model's blank disk) and the
Commodore's (`main/main.c`): `k <n>` tap key n (row * 8 + column: 1 RETURN,
63 RUN/STOP; 64-87 the C128's K rows, 88 RESTORE, 89 CAPS LOCK, 90 40/80),
`K <n> 0|1`, `T <text>` type, `j <port> <bits>` joystick, `R <model>`
restart as 0 C64, 1 C128. 89 and 90 lock: `K 90 1` then `K 90 0` toggles.

## M5Stack Tab5

The same code also builds for the M5Stack Tab5 (ESP32-P4, target esp32p4),
beside the MaTouch build, on `/dev/cu.usbmodem3101`:

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- Its settings: this project's `sdkconfig.defaults`, then
  `../esp32emu/boards/m5stack_tab5/sdkconfig.defaults` (`CMakeLists.txt`),
  into `build-tab5/sdkconfig` (delete it and `reconfigure` after changing
  either). Board details and lessons: `../esp32emu/CLAUDE.md`, "The two
  boards".
- The Tab5 is shared by the projects like the MaTouch: after another
  project, a full `flash` (partition table and ROMs). The MaTouch build
  afterwards recompiles everything once (shared `managed_components/`).
- Measured on the Tab5 (2026-10-17): C64 at BASIC: 100% (paced by the speaker at 30789 Hz), core 1 19% busy (the S3: ~40%), typing through the console; the picture 960x720. Sound on by default
  (ES8388); the console stays on USB.
- Not checked by eye yet: the picture on the panel itself (orientation),
  touch, sound, the Tab5 Keyboard's keys, Bluetooth pairing.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Commodore as an `emu_machine_t`: memory, ROMs, start, key map, joystick, drives, status, console |
| `main/rom_store.c` | ROMs from the `cbm_roms` slots, else the SD card's `roms/` |
| `main/settings.c` | NVS `cbm`: `model`, `joystick` |
| `components/cbm/` | The C64 and C128, platform independent (`include/cbm.h`) |
| `components/cbm/host/` | Host build: `cbm_host` (see Testing) |
| `roms/` | the nine ROM images (see `roms/README.md`; not in git) |
| `partitions.csv` | nvs, phy, factory 8 MB, cbm_roms 256K (type 0x40), storage |
| `docs/images/` | README screenshots |

## The core (`components/cbm`)

- `cbm.c`: the C64 PLA (32 modes), memory map as 256-byte pages for the
  6502 core, the 6510 port ($00/$01 kept in RAM for reading; writes through
  the bus; bits 3/6/7 keep their last driven level), I/O dispatch, the CIA
  wiring, the frame loop (312 lines of 63 cycles: per line the CPU runs to
  min(line end, next CIA event); after a run, events by the instruction's
  poll cycle are processed and IRQ re-polled), the machine clock.
- **Clock**: `cbm.clock` (1 MHz cycles) is the time of the CIAs, VIC, SID
  and drives. At 1 MHz it equals the CPU's counter; at the C128's 2 MHz
  the counter runs twice as fast from `fast_cpu0`/`fast_clock0`
  (`cbm_sync_clock()` in bus callbacks and after runs, `cbm_cpu_time()` the
  other way). A `$D030` write only requests the switch; it happens between
  runs, and going back to 1 MHz steps the CPU's counter (and poll/NMI
  times) back to the clock.
- `cia.c`: lazy timers per timer (`cia_timer_t`): value at `last`, start
  and stop take effect 3 cycles after the write (`counting`, `stop_at`), a
  force load shows 2 cycles after and the counter holds a cycle; it never
  reads 0 (it underflows and reloads there); timer B counting A checks for 0
  a cycle after each A underflow and counts a cycle later (`b_events`); a
  one-shot bit counts in either of the two cycles before the underflow; the
  IRQ line follows a flag a cycle later (old 6526); an IMR write counts
  from the next cycle. `next_event` is only what a CPU must see (an
  enabled interrupt, the shift register's byte); other underflows wait for
  register reads, and many at once are counted in one step. Port accesses
  don't touch the timers. All from the Lorenz tests (see Lessons).
- `vic.c`: 6569, cycle stealing as a per-line mask (bad line cycles, sprite
  DMA) that the CPU's reads check (`stall_from`, `vic_cpu_stall()`), lines
  drawn as the beam passes (`vic_catch_up()`) into a 400x300 RGB565 frame,
  unchanged lines skipped by a key of their inputs, sprites, collisions.
  `vic_ram`/`vic_chargen`/`vic_color`/`vic_rom_chars`/`vic_chars_all_banks`
  are the memory it sees (the C128 changes them).
- `sid.c`: writes queued with their times, rendered on the speaker task
  (32 cycles a sample, 30789 Hz); voice 3's oscillator and envelope also on
  the emulation side for `$D41B/$D41C`.
- `c128.c`: the MMU (CR, PCRs/LCRs, MCR, RCR, page pointers, `$FF00-$FF04`
  in every configuration), the 8502's page map (rebuilt only when the
  configuration changed), the Z80's map and I/O, the CPU switch (the Z80
  boots first; the 8502 does its reset when it first gets the bus), the
  fast serial hook.
- `vdc.c`: 8563 registers and 64 KB RAM (16 KB addressing unless R28 bit
  4), block copy/fill on R30, the picture once a frame while shown: 800x200,
  each 8-pixel cell 10 wide.
- `drive.c`, `via.c`, `gcr.c`: the drives (see Drives), D64/D71/G64 to GCR
  tracks and back.
- `cart.c`, `prg.c`, `input.c` (matrix, K rows, locking keys, typing).

## Drives

- Each drive: 2 KB RAM, ROM (1541: 16 KB at $C000 mirrored at $8000;
  1571: 32 KB at $8000), VIA 1 at $1800 (bus; 1571: PA0 track 0, PA1 fast
  serial direction, PA2 side, PA5 2 MHz, PA7 BYTE READY), VIA 2 at $1C00
  (mechanism, GCR data), 1571: WD1770 at $2000, CIA at $4000.
- Drive time: its own counter, `base_c`/`base_d`/`speed` map computer time
  to it (`mul_frac`: no 64-bit division). They run behind the computer:
  `drives_run()` at every line end and on reads of `$DD00`; the computer's
  bus changes (and the C128's fast serial bytes) are a timestamped history.
- A run stops at the next of: the computer's time, a VIA timer interrupt,
  the CIA's next event, the next history entry, the next disk byte only if
  the CPU needs it on time (SO enabled or VIA 2's CA1 interrupt), the idle
  loop (`stop_pc` $EC9B), a wait-for-byte loop (`stop_on_spin`: `BVC *`, or
  the 1571's `BIT $180F / BMI`; then the clock jumps to the byte).
- Idle: at $EC9B (both DOSes) with `$7C` clear, ATN high and no bus change
  pending: motor off, the drive isn't run until the bus moves; motor on, it
  jumps to its next timer interrupt.
- Disk bytes otherwise come lazily when the CPU touches VIA 2.

## Persistent state

- NVS `cbm`: `model`, `joystick`. NVS `cbmdisks`: the four slots' files.
- SD card: `cbm/` (all images; made if missing), `roms/` (optional ROMs).
- A program in its slot loads 4 s after every start (and at once when
  inserted while running).

## Memory budget (measured on the board)

- PSRAM (6.6 MB heap): RAM 128 KB, ROMs ~150 KB, VIC picture 240 KB, VDC
  picture 320 KB, reserved first; each disk is its image plus its GCR
  tracks (~250 KB a side). ~4 MB free with the setup screen open.
- Internal RAM: ~43 KB free while running (min ~26 KB) with the 32 KB
  instruction cache (`sdkconfig.defaults`); the 6502 core is in flash, its
  read-only data (the opcode jump table) in DRAM (`linker.lf`).
- Core 1: C64 at BASIC ~40% busy; KERNAL loading a 150-block file from the
  1541 96-100% speed; C128 at BASIC ~36%; CP/M at its prompt ~47%; CP/M
  loading from the 1571 (2 MHz) 70-90% speed.

## Lessons

- **The 6502 core's cost was its structure, not where it ran**: a call per
  instruction and 64-bit counters in memory per bus access made ~175 ESP32
  cycles per instruction (IRAM didn't help: 177). `m6502_run()` runs many
  instructions with the registers in locals and a 32-bit cycle count, the
  machine running to its next event: ~127. What then counted: the opcode
  jump table read from flash through a data cache that the video keeps
  flushing (`noflash_data` in the core's `linker.lf`), and a 32 KB
  instruction cache (busy drive: 74% → 93% speed).
- **Find the time with the profiler** (`emu_console.py profile`): guesses
  were wrong twice (IRAM, inlining).
- **No 64-bit division in hot paths**: the drive clock conversion was a
  libgcc call per drive instruction.
- **The CPUs stop only for events that matter**: CIA underflows that only
  set a flag (the 1571's shift-register timer: thousands a frame) were runs
  of a few cycles; VIA free-running timers likewise (`via_next_irq()`).
- **`m6502_set_overflow()` only between runs**: from a bus callback inside
  a run the V flag was lost (the run's locals), a byte skipped: disk bytes
  are caught up in callbacks only while SO is off.
- **Interrupt timing (Lorenz irq/nmi)**: an IRQ is seen if it came by the
  instruction's last cycle but one (`poll_cycle`, `m6502_repoll()`); a taken
  branch that doesn't cross a page polls a cycle earlier; an NMI by the
  fourth cycle of a BRK or IRQ sequence takes over its vector; NMIs carry
  their cycle (`m6502_nmi_at()`).
- **The 6510 port**: bits 3, 6, 7 read back their last driven level as
  inputs (Lorenz cpuport).
- **C128, VICE's port semantics**: `$01` bit 0 low gives the CPU the second
  colour RAM bank, bit 1 low the VIC; bit 2 low puts the character ROM in
  all four VIC banks; CAPS LOCK (bit 6 low) selects the second character set.
- **C128 Z80 map (VICE)**: in bank 0 it reads its BIOS at $0000-$0FFF even
  with CR = $3F, its writes there go to the RAM at $D000-$DFFF, and IN/OUT
  to $0000-$0FFF reach that RAM; with I/O selected (CR bit 0 low) $1000-$13FF
  is the colour RAM. Without that, CP/M's 40-column colour writes landed on
  its key table (every key was ^D).
- **The Z80 core looked ahead with `z80_peek()`** for DD/FD prefixes: blind
  to `read_special()` (all IX/IY instructions were wrong on the C128).
- **D71 side 2 headers say tracks 36-70** (40-track D64s too): with 1-35
  the 1571 took the disk as single-sided (664 blocks free).
- **The 1571 waits for its WD1770 to go BUSY** after a command, and polls
  BYTE READY on VIA 1 PA7 (cleared by VIA 2 port accesses).
- **A ROM may have a 4 KB block starting with $FF bytes** (the 1571's):
  the "erased slot" check now allows a quarter of them.
- **Typing under CP/M** needs keys held longer (one row scanned per
  interrupt), chosen once per key: the CPUs take turns during a disk access.
- Host scripts: give the C64 ~150 frames and the C128 ~400 before typing;
  typing while BASIC is still printing loses keys.

## Testing

- **Host build**: `make -C components/cbm/host`, then
  `build/cbm_host <c64|c128> [frames] [out.ppm]`; options in `host_main.c`
  (`CBM_DISK8/9`, `CBM_NEWDISK`, `CBM_CART`, `CBM_PRG`, `CBM_SCRIPT` with
  type, key, joy, shot, insert, eject, cart, prg, peek, dump, dumpram, text,
  reset; `CBM_STATUS`, `CBM_TRACE`, `CBM_Z80TRACE`, `CBM_WAV`; `-DCBM_MMU_LOG`,
  `-DCBM_FAST_LOG`, `-DCBM_Z80IO_LOG`).
- **The Lorenz test suite 2.15** (VICE's `testprogs/general/Lorenz-2.15`,
  Disk1-4.d64): load each disk's first program (`start`, `beqr`, `irq`,
  `aneb`), RUN, press a key now and then, and read the screen with `text`
  every 1000 frames; all four disks pass (old CIA), disk 4 ends with "test
  suite 2.15+ completed - ok".
- **The 6502 tests** (`../esp32emu/components/m6502/test`): Klaus Dormann's
  functional test passes (30646177 instructions); `opcodes` prints every
  opcode's cycles (compare before and after core changes).
- Verified on the host: C64 BASIC, SAVE/LOAD/directory on a new D64 (the
  image decoded back), a 150-block KERNAL load (bytes compared); C128 boot
  in 40 and 80 columns, GO64, FAST (2.15x), BSAVE/BLOAD of three 248-block
  files on a D71 (side 2 used, bytes compared), CP/M 3.0 boot and DIR (the
  28may87 system disk from zimmers.net); SID tone at the right pitch.
- On the board: C64 and C128 at 100% at BASIC, the Lorenz disk 1 running
  from the Program slot, the long KERNAL load, C128 DIRECTORY of a D71 in
  40 and 80 columns, a BASIC program in 80 columns, CP/M boot and DIR, the
  Disks page, uploads and inserts through the web API.
- Not verified yet: sound on the speaker, a BLE keyboard on the Commodore,
  joysticks, cartridges, G64 and T64 images, fast loaders, games, demos,
  the web page's drop area.

## Open items

- A busy 1571 at 2 MHz runs the C128 at 70-90%: the drive's runs stop at
  every disk byte while SO is enabled.
- No WD1770 MFM, no VDC interlace/light pen, FAST doesn't slow I/O.
- PAL only, 6581 only; no datasette, REU, printer, 1351 mouse.
- Public on GitHub (`github.com/rlucencic/esp32cbm`, pushed to `main`).
  Licence: MIT (`LICENSE`).

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing. Screenshots: a new file each time, never over an
  existing one.
