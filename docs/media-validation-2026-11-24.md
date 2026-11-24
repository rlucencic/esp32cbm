# C64 media validation, 2026-11-24

The host and MaTouch use PAL C64 mode, the same C64 and 1541 ROMs and
drive 8 for floppy images. The emulator baseline is `537d839` with shared
runtime `4ae5270`. The test media and full captures are local to
`build/roadmap-media-2026-11-24/`.

## Fixtures

| Medium | Source and preparation | SHA-256 |
| --- | --- | --- |
| Hessian 1.2 G64 | Convert the author's D64 with `perl g64conv.pl hessian.d64 hessian.g64` using g64conv at `a488ede575e58094c2cd1483b6d95a366dd06343`. [Hessian release and manual](https://cadaver.github.io/games.html) · [g64conv](https://github.com/markusC64/g64conv/tree/a488ede575e58094c2cd1483b6d95a366dd06343) | `0adb492f47cf26757f2be1841db64fea476dc45ace8cb3bcd6ed571bfdc46383` |
| ARE-TYPE T64 | Wrap the existing 30,530-byte replay PRG in a T64 with one PRG directory entry. The start is `$0801`, the end is exclusive and the data offset is 96. [Pinned PRG and run](aretype-validation-2026-11-08.md#game-and-builds) | `a01b8b53a4e8215ae9104177281a269d1614a7e5db377c045d3b2276c412257d` |
| EasyFlash example CRT | Covert Bitops' `exampleef.crt` has mapper 32 and 64 distinct banks. [Pinned C64 Game Framework example](https://github.com/cadaver/c64gameframework/tree/c634f6fa7004cc5bfb14df14dee6f9fa3fe20b1b) | `3e1d6e2de8e7773cf6073bdf5c2a72073f1564ed38c73d9844ae57c22e87f239` |

## Host results

- **T64.** Run the ARE-TYPE control for 6,000 frames with the same inputs
  as the earlier replay. Change only `CBM_PRG` from the PRG to the T64.
  Six frame captures, the corresponding RAM dumps, the final frame and
  the PCM WAV match byte for byte. Samples of the game's state byte
  include the title (12), active play (93) and game over (5).
- **EasyFlash.** Boot with `CBM_CART` set to the example. At frame 1490,
  the game shows the player on the starting platform. Hold joystick 2
  right from frame 1500 through 1599. At frame 1630, the player has
  reached the ramp and ladder and the HUD shows 95 health. The cartridge
  path was exercised through boot and movement.
- **G64 writeback.** On a G64 made from the Metal Warrior 1.6 release,
  save a scratch BASIC program as `ROADMAP`, eject and reinsert the G64,
  clear BASIC, and load and run the program. It prints `media ok`. After
  conversion back to D64, exactly three logical sectors differ from the
  pristine round trip (BAM, directory and file data). [Metal Warrior 1.6 release](https://cadaver.github.io/games.html)
- **Hessian.** Type `load ":*",8,1` at frame 200. With its default
  loader, both the original D64 and the converted G64 are still black
  at frame 10200 (200 seconds past the command). With Space held from
  frame 320 through 1999 to select the documented fallback, the splash
  is visible by frame 2400 (44 seconds after the command starts), the menu
  at frame 6000, and the blue garage scene at frame 9400. Use joystick 2
  fire for 25 frames every 200 frames from frame 5600 to progress through
  the title, menu and dialogue. Add right at frames 8501–8658 and
  9401–9800, and up at 9050–9300. The default loader attempts leave the
  D64 and G64 unchanged.

## MaTouch results

The ESP-IDF 5.5.6 build used the MaTouch target with autostart and the
UART console through the speaker configuration. These settings were
local to the test build.

- The default Hessian G64 load stayed black after 65 seconds. The serial
  status repeatedly reported PC `$F455` and drive 8's motor at track 30
  while the emulator continued running at about 50 fps. The image
  downloaded from the board matched the uploaded G64 byte for byte.
- With Space held during startup, the MaTouch had reached the Hessian
  splash by the 65-second capture. The status sample was 47 fps during
  disk activity. All 120,000 guest pixels matched the host splash when
  compared in RGB565 at the guest's native 400×300.
  The MaTouch also reached the press-fire screen and the opening scene
  after joystick 2 input. The press-fire and scene checkpoints were
  captured separately because the next disk load was still active after
  the menu selection. The fallback run left the G64 unchanged.
