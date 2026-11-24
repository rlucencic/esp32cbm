# esp32cbm roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Broaden the C64 media and loader sample.** Use the [ARE-TYPE
   replay](docs/aretype-validation-2026-11-08.md) as a control. Add one IEC
   fast loader on a `.G64`, one `.T64` program and one banked cartridge. Record
   title and gameplay checkpoints, load time and any disk writeback, then
   repeat the heaviest case on the MaTouch.

2. **Profile the C128 while CP/M boots.** Measure the 1571 at 2 MHz with
   `components/cbm/drive.c` counters for byte-ready and SO events. Compare boot
   time and real-time pace on the host and boards. Require matching sector
   reads and C128 burst-load behavior before retaining a scheduling change.

3. **Check 40- and 80-column use on the Tab5.** Use a physical keyboard in C128
   BASIC and CP/M, switch the displayed output, and check a 40-column SID tone
   on the speaker. Record which keyboard paths were exercised and verify that
   changing output leaves the mounted disk and guest session intact.
