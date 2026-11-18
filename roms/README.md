# Commodore ROM images

Put the ROM images here (not part of this repository). `idf.py flash` writes
them into 16 KB slots of the `cbm_roms` flash partition; only the ones for the
models you want are needed.

| File | What | Size | zimmers.net name |
|---|---|---|---|
| `c64basic.rom` | C64 BASIC V2 | 8 KB | `basic.901226-01.bin` |
| `c64kernal.rom` | C64 KERNAL (revision 3) | 8 KB | `kernal.901227-03.bin` |
| `c64chargen.rom` | C64 character set | 4 KB | `characters.901225-01.bin` |
| `1541.rom` | 1541-II drive DOS | 16 KB | `1541-II.251968-03.bin` |
| `c128basic.rom` | C128 BASIC 7.0 ($4000-$BFFF) | 32 KB | `basic.318022-02.bin` |
| `c128kernal.rom` | C128 editor, Z80 BIOS and KERNAL ($C000-$FFFF) | 16 KB | `kernal.318020-05.bin` |
| `c128chargen.rom` | C128 character sets (C64's, then the C128's) | 8 KB | `characters.390059-01.bin` |
| `c128c64.rom` | the C128's C64 mode: BASIC V2 and KERNAL | 16 KB | `64c.251913-01.bin` |
| `1571.rom` | 1571 drive DOS 3.0 | 32 KB | `1571-rom.310654-05.bin` |

The C64 needs the first three (and `1541.rom` for disks), the C128 the four
C128 ones (and `1571.rom` for disks).

A file of the same name in the `cbm/roms` folder on the SD card (with the
cartridges) is used instead of the flashed one. A card set up before
2026-11-17 has such files in a shared `roms` folder: esp32emu's
`tools/sd_folders.ps1 E:\` (PowerShell 7, the card in a card reader) moves
them there.

The images are on zimmers.net, under `anonftp/pub/cbm/firmware/`:
`computers/c64/` (the C64's, and `64c.251913-01.bin`), `computers/c128/`,
`drives/new/1541/` and `drives/new/1571/`.
