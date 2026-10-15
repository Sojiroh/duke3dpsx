# Duke Nukem 3D for PlayStation (PS1)

A port of the Duke Nukem 3D v1.5 source release to the original PlayStation, built on the
Build engine code from that release. The renderer is a GPU back end (textured quads in
place of Build's column rasteriser), with textures streamed from the CD into VRAM.

The game data is not distributed. You need your own `DUKE3D.GRP` (v1.3D); `tools/mkdata.py`
converts it into the `DUKE.DAT` asset pack that goes on the disc. The source code is the GPL
release published by 3D Realms.

## Requirements

- **MIPS toolchain**: `gcc-mipsel-none-elf-12.3.0-linux.zip` unpacked into `tools/gcc`
  (so that `tools/gcc/bin/mipsel-none-elf-gcc` exists).
- **PSn00bSDK v0.24**: `PSn00bSDK-0.24-Linux.zip` from
  <https://github.com/Lameguy64/PSn00bSDK/releases>, unpacked into `tools/sdk`
  (so that `tools/sdk/PSn00bSDK-0.24-Linux/` exists).
- **CMake** (3.21 or newer).
- **Python 3** with `numpy` and `Pillow` (used by the build tools and `tools/emu.py`).
- **PCSX-Redux** (optional, for testing): place the AppImage at
  `tools/redux/PCSX-Redux-HEAD-x86_64.AppImage`.

`tools/gcc`, `tools/sdk`, `tools/redux`, `build*/` and `upstream/` are git-ignored.

## Building

```sh
DUKE_GRP=/path/to/DUKE3D.GRP ./build.sh iso
```

This configures CMake, runs `tools/mkdata.py` to produce `build/data/DUKE.DAT` and
`build/data/data_index.h`, builds the executable and creates the CD image:

- `build/duke3d.bin` and `build/duke3d.cue`
- `build/duke3d.exe` and `build/duke3d.elf`

`./build.sh` alone builds only the executable. `./build.sh clean` removes the build directory.

Environment variables read by `build.sh`:

| Variable | Default | Meaning |
| --- | --- | --- |
| `DUKE_GRP` | `$HOME/Downloads/DUKE3D/DUKE3D.GRP` | Path to `DUKE3D.GRP` (v1.3D) |
| `BUILD_DIR` | `build` | Build directory, relative to the project root |
| `DUKE_VIEWER` | `OFF` | `ON` builds the engine map viewer (`tools/viewer/viewer_main.c`) instead of the game |
| `DUKE_SF2` | empty | Optional SoundFont (`.sf2`) for the music instruments. Empty uses the built-in synthesised bank. It is never searched for; the file is passed to `tools/mkdata.py --sf2` and its samples are stored in `DUKE.DAT`, so check its licence before distributing the disc image |
| `EXTRA_CFLAGS` | empty | Extra compiler flags, e.g. debug switches |

Debug switches (set through `EXTRA_CFLAGS`):

- `-DPS1_NOAUDIO=1`: disables all sound and music.
- `-DPS1_WARP=N`: skips the logo, title and menu and starts episode *E*, level *L* with
  `N = E*10 + L` (for example `-DPS1_WARP=11` starts E1L1 at skill 2).
- Other debug-only switches: `-DPS1_PROF` (per-frame timing, engine section timers and texture-cache /
  primitive-buffer statistics every 120 frames; the engine timers exist only under this flag),
  `-DPRIM_RESERVE=<bytes>` (test the 2D primitive reserve), `-DPS1_HOLES`
  (clears the 3D view to white to expose uncovered pixels), `-DPS1_WARPX/Y/ANG/SECT` (start
  position override, god mode), `-DPS1_SAVETEST=<seconds>` (memory card round-trip test).

Flags and options are cached per build directory by CMake: `build.sh` passes `DUKE_GRP`,
`DUKE_SF2`, `DUKE_VIEWER` and `EXTRA_CFLAGS` only when the variable is set, so running
`./build.sh` again in a configured directory keeps them (`EXTRA_CFLAGS=""` clears the flags).
Use a separate `BUILD_DIR` for each flag set:

```sh
BUILD_DIR=build-noaudio EXTRA_CFLAGS="-DPS1_NOAUDIO=1" ./build.sh iso
```

## Running

### Emulator

Use `tools/emu.py` with PCSX-Redux (see Testing below). It boots the CD image by default
(`build/duke3d.cue`), or runs an executable with `--exe`.

### Real hardware

Burn `build/duke3d.bin` and `build/duke3d.cue` to a CD-R. The disc contains `SYSTEM.CNF`
(`BOOT=cdrom:\DUKE3D.EXE;1`), `DUKE3D.EXE` and `DUKE.DAT`. A modded console or a boot loader
is required to run unsigned discs. Saves are intended for a memory card in slot 1; see
Status.

## Controls

Default pad mapping (from the header comment of `src/game/ps1_control.c`). There is no
keyboard or mouse; the original bindings are replaced by this fixed mapping.

In game:

| Input | Action |
| --- | --- |
| D-pad UP / DOWN | Move forward / backward |
| D-pad LEFT / RIGHT | Turn left / right (strafe left/right while SQUARE is held) |
| Left stick | Y: forward/back, X: strafe (analog) |
| Right stick | X: turn, Y: look up/down (analog; mouse-style aiming is always on) |
| R1 | Fire |
| L1 | Jump |
| R2 | Toggle auto-run (the game prints `RUN MODE ON` / `RUN MODE OFF`; the default is run) |
| L2 | Crouch |
| CROSS | Open / Use |
| CIRCLE | Jump (alternative) |
| TRIANGLE | Next weapon |
| SQUARE | Strafe modifier |
| R3 | Center view |
| L3 | Quick kick |
| START | Escape (opens and closes the menu) |
| SELECT | Modifier. A tap with nothing else pressed toggles the Map. |

SELECT combinations:

| Combination | Action |
| --- | --- |
| SELECT + TRIANGLE | Previous weapon |
| SELECT + LEFT / RIGHT | Inventory left / right |
| SELECT + CROSS | Use inventory item |
| SELECT + UP | Jetpack |
| SELECT + DOWN | Medkit |
| SELECT + L1 | Shrink screen |
| SELECT + R1 | Enlarge screen |
| SELECT + START | Pause |
| SELECT + L2 | Aim up (held: keeps aiming, no auto-centre) |
| SELECT + R2 | Aim down (held: keeps aiming, no auto-centre) |

Menus and screens:

| Input | Key |
| --- | --- |
| CROSS | Enter |
| TRIANGLE, START | Escape |
| SQUARE | Space |
| D-pad | Arrow keys (auto-repeat) |
| SELECT | Tab |
| L1 / R1 | Page Up / Page Down (menu up / down) |

## Project layout

- `src/ps1/`: PS1 platform layer. `main.c` (entry point), `video.c` (double-buffered display
  list, flip, validation), `pak.c` (reads `DUKE.DAT` from CD, sync and async), `cdrom.c` (CD
  command layer, see Technical notes), `kfile.c`
  (file API over the pack), `pad.c` (controller state), `spu_fx.c` (sound effects on SPU),
  `spu_music.c` (music sequencer), `memcard.c` (memory card files via the BIOS), `audio_test.c`.
- `src/engine/`: Build engine. `engine.c` (globals, init, board loading) with
  `parts/engine_core.c` (visibility and collision) and `parts/render.c` (GPU renderer),
  `texcache.c` (VRAM texture cache with CD streaming), `palette.c` (CLUT cache, shading, tint).
- `src/game/`: ported game code. `game.c` (`duke_main`), `ps1_control.c` (pad to input
  mapping), `ps1_platform.c` (network, timer and cache stubs), `sounds.c`, `menues.c`,
  `savegame.c` (save game state coding, see Technical notes), `loadscreen.c` (loading screen with a
  progress bar, see Technical notes), and the rest of the game logic.
- `tools/`: build and test tools.
  - `mkdata.py` builds `DUKE.DAT` and `data_index.h` from the GRP (tiles, palettes, maps,
    CON scripts, sounds as SPU-ADPCM, music).
  - Per-level tile groups: after the global tiles (still the fallback for every tile), `TILEDATA`
    holds one contiguous copy per map of the tiles that map can ask for (map picnums, their
    `picanm` animation ranges, the `cachespritenum()` extras, the HUD/weapon tiles; multi-piece
    tiles excluded). Disc order inside a group: all full-res pieces first, then all half-res
    copies; within each part the tiles are ordered by distance from the player start (the always
    needed HUD/font/weapon tiles, then by hop count of the nearest sector using the tile: one hop per
    portal `nextsector` link or SE 7 teleporter jump, animation frames and `cachespritenum()` extras
    inheriting their base tile's hop count, tiles used only by unreachable sectors last; ties by
    tile number), so the preload and the streaming read the nearest tiles first. The
    `TILEMAP_<MAP>` pack entry lists `(u16 tile, u16 sector, u16 halfsector)` records sorted by
    tile, sectors relative to
    `TILEDATA`. `loadpics_group()` (engine.c, called from `enterlevel()` and `loadplayer()`) re-reads
    `TILEIDX` and overlays the records, so streaming and the preload read one disc area per level
    instead of seeking over the 20 MB of tiles. Costs about 70 MB of disc space (`DUKE.DAT` ~99 MB).
  - `grp.py` reads GRP archives. `spuadpcm.py` encodes SPU-ADPCM. `mkmusic.py` converts music.
  - `emu.py` runs the headless emulator test harness.
  - `viewer/viewer_main.c` is the engine viewer used with `DUKE_VIEWER=ON`.
- `upstream/`: reference source from the 3D Realms release (read-only).
- `iso.xml`, `system.cnf`: CD image layout and boot configuration.

## Testing

`tools/emu.py` runs the game headless in PCSX-Redux and executes a list of actions in order.

```sh
python3 tools/emu.py --timeout 300 --exe build/duke3d.exe \
    "wait 1650" "press CROSS 10" "wait 150" "press CROSS 10" "wait 150" "shot menu" "quit"
```

Options:

- `--iso FILE`: CD image to boot (default `$BUILD_DIR/duke3d.cue`).
- `--exe FILE`: executable to run.
- `--out DIR`: output directory for screenshots (default `$BUILD_DIR/shots`).
- `--timeout S`: seconds before the run is aborted (default 300).
- `--memcard FILE`: memory card image for slot 1.
- `--watchdog N`: quit after N stall samples (default 60). The first sample comes after 240 vsyncs
  without a new game frame, then one every 40 vsyncs; the count restarts whenever the game
  advances a frame.
- `--dynarec`: enable the dynamic recompiler in PCSX-Redux.

Actions:

| Action | Meaning |
| --- | --- |
| `wait N` | Wait N frames |
| `press BTN[+BTN] N` | Hold the buttons for N frames, then release |
| `hold BTN[+BTN]` | Press and keep held |
| `release BTN[+BTN]` | Release |
| `shot NAME` | Save a screenshot as `NAME.png` in the output directory |
| `vram NAME` | Dump the whole VRAM as raw 16-bit pixels to `NAME.vram` in the output directory (no PNG) |
| `pc N` | Print the CPU state (`EXCEPTION ... (sample)` lines) every vsync for N vsyncs |
| `prof N` | Sample the PC over N vsyncs and print a PROFILE |
| `quit` | Exit the emulator |

Buttons: CROSS, CIRCLE, SQUARE, TRIANGLE, UP, DOWN, LEFT, RIGHT, L1, L2, R1, R2, START, SELECT.

The game's TTY output is printed. A crash prints `EXCEPTION pc=... (symbol)`. A stalled
frame counter prints `WATCHDOG`. Symbols and the watchdog address come from the `.elf` next to
`--exe` (or `$BUILD_DIR/duke3d.elf` with `--iso`). The harness exits with code 0 when the script
completes or after `quit`, 3 after a CPU exception (`crash.png`), 4 after the watchdog fired
(`hang.png`) and 124 on `--timeout`.

Boot to the 3D Realms screen takes about 1520 vsyncs in the emulator. The first CROSS skips
that screen, the second skips the title animation and shows the main menu (sequence above).
Loading E1L1 takes about 2000 vsyncs.

## Technical notes

- **Renderer**: walls, floors and sprites are drawn as textured GPU quads. Build's column
  rasteriser is not used. `rotatesprite` and the palette tint overlay are also GPU operations.
  The 3D emitters stop 6 KB before the end of the primitive buffer so that the weapon, HUD, menus
  and the tint always fit; dropped 3D quads are counted (`PROF tex:` line under `-DPS1_PROF`).
  Non-power-of-2 tiles that repeat are split exactly at the texture period, so they tile seamlessly.
- **Textures**: a VRAM tile cache (x 256..1023, 12 pages of 256x256) streams tiles from `DUKE.DAT`
  on the CD. Wall/floor tiles live in power-of-two blocks (they repeat through the GPU texture window);
  sprite-class tiles are packed on shelves, which halves their VRAM waste. Misses are served in
  coalesced reads: one CD command fetches every queued tile inside a 32-sector window plus a few
  sectors of read-ahead while evictable VRAM exists; each level's tiles are stored contiguously on the
  disc (`TILEGRP_<MAP>`, ordered by map distance from the player start), so seeks stay short. Tiles of
  the sectors around the player are prefetched before they are seen (`tex_prefetch_sector`). World
  sprites of 48 px or more (enemies, props, effects; never HUD, weapons, fonts or screens) are stored
  and drawn at half resolution (`TEX_SPRITE_HALF`, as the official 1997 port did); wall/floor
  half-resolution copies are used only when the recent working set (tiles used in the last 120
  frames, plus locked tiles) fills more than 60% of the cache. A sprite whose next animation frame is
  still streaming keeps showing its last resident frame; in-game HUD/weapon draws never block on the
  CD, multi-piece tiles stream asynchronously too, and nothing in the play loop issues a blocking CD
  read: music is held in RAM for the whole song, each level's sound effects are precached from a
  contiguous per-level group (`SNDGRP`/`SNDMAP`, chosen from the CON actor scripts and the map's
  ambient sounds), and a sound that is not resident is loaded asynchronously and played when it
  arrives (dropped if older than 40 vsyncs).
- **Display**: the GPU runs in the 256-pixel-wide mode (framebuffers 256x240) so the texture cache gets
  the extra VRAM column; the game and engine still work in 320x240 logical coordinates, which the
  renderer scales at primitive emission (`SCR_X`, `SCR_X3D`). The 3D view keeps its 160 columns.
- **Sound effects**: VOC files are converted to SPU-ADPCM at build time and played through
  the audiolib FX API on the SPU.
- **Music**: a MIDI-style sequencer driven by the Timer 2 interrupt. Song blocks are streamed
  from the CD with asynchronous reads. Instruments are an SPU-ADPCM sample bank built by
  `tools/mkmusic.py`: synthesised by default, or taken from a SoundFont passed with `--sf2`
  (`DUKE_SF2`). Looping sounds are padded to a whole ADPCM block by stretching the loop by fewer
  than 28 samples (no zero gap), and their first block is encoded so it does not depend on the
  samples before it, so the loop point does not click.
- **CD access**: all CD commands are issued from the main thread, while the drive is idle
  (`pak.c`: no asynchronous read in flight and the `CdlPause` that ends a read completed). The
  only command sent from interrupt context is that `CdlPause`, by the SDK's sector callback; the
  completion callbacks of `pak.c` only set flags. `src/ps1/cdrom.c` is PSn00bSDK v0.24's `libpsxcd`
  `common.c` with one change: `CdCommandF()` writes the command parameters and the command byte
  as one indivisible, interrupt-masked, branch-free sequence. The stock version can be interrupted
  between the first parameter byte and the command byte; the CD interrupt handler then switches
  the controller to register index 1, the remaining parameter bytes are written to the interrupt
  mask register instead, and the controller stops answering (`BUSYSTS` stuck, the game hangs in
  `CdCommandF`). PCSX-Redux triggers it on its own (its deferred "start next command" event runs
  a stale `CdlPause` with the first parameter byte and answers INT5 in the middle of the sequence).
- **Save games**: 10 slots on memory card 1, written through the BIOS file API (`bu00:`, see
  `src/ps1/memcard.c`). A file is 1 to 15 blocks (8 KB): the standard PS1 header and icon, a
  128-byte meta block (with a save generation number), then the game state
  (`src/game/savegame.c`) stored as the difference from the level's original `.MAP` data and
  from the values `spawn()` would give, zero-run-length coded (E1L1 at the start is about 16 KB,
  3 blocks). The CON script is not saved; a checksum rejects saves made by a different CON/build.
  Each slot has two possible file names, `BASLUS-00000DUKE<n>` and `...DUKE<n>B`. A save is
  written to the name not currently in use, read back and checksum-verified, and only then is the
  old file erased, so a failed or interrupted save never loses the previous one (if both exist at
  start-up the newer one that verifies wins and the other is erased). If the card has no room
  for a second copy the old file is replaced in place and a warning is shown. Missing and full
  cards produce an on-screen message (`NO MEMORY CARD IN SLOT 1`, `MEMORY CARD FULL`, ...).
  The Timer 2 interrupt (music) is masked while the card is accessed because the BIOS card
  routines stop working with it enabled. Debug: `-DPS1_SAVETEST=<seconds>` saves slot 0 after that
  many seconds in game, perturbs the state, saves again (safe replace), perturbs again, loads and
  prints state checksums (`SAVETEST ...` lines); `-DPS1_SAVETEST_LOADONLY=1` only loads slot 0;
  `-DPS1_SAVETEST_KEEPOLD=1` skips erasing the old copy (simulates an interrupted save).
- **Loading screen** (`src/game/loadscreen.c`): the boot (~1400 vsyncs after the screen comes up, 70%
  of it compiling the CON files), the level loads, loading a saved game and saving show the game's
  own big red `menutext()` letters (on black, or on the `LOADSCREEN` art for levels) and a progress
  bar drawn with flat palette colours (`rend_fillrect()`). The bar is a list of stages
  (`loadscreen_stage(end_pct, expected_bytes)`), each ending at a percentage proportional to its
  measured duration. While a stage runs the bar follows the CD bytes read by `pak_read()` (the
  `pak_progress` hook; boot stages), or the stage reports itself with `loadscreen_progress()` (CON
  compile by source position, the tile preload, card read/write). A redraw presents one frame
  (`nextpage()`) and only happens every 2 percent (it costs about half a vsync of waiting); it only draws tiles that are resident
  (fonts and `LOADSCREEN` are locked in the texture cache; `tex_flush()` keeps locked tiles, so a
  level load no longer re-reads the fonts), and `loadscreen_begin()` drains the streaming queue
  (`tex_quiesce()`) so no streaming read runs while the screen is up. Debug: with `-DPS1_PROF` every
  stage prints its duration and CD bytes (`LS: stage -> N% took V vsyncs, B CD bytes`), which is what
  the percentages in `game.c`, `premap.c`, `menues.c` and `savegame.c` were derived from.
- **Memory**: the console has 2 MB of RAM. The SDK heap (`InitHeap` at `_end`, a few KB used by
  `CdSearchFile`) and the stack share the space above `.bss`. `build.sh` enforces that the end of
  `.bss` (`_end`) stays at or below 0x801F8000, keeping 32 KB for them (it prints
  `RAM: _end=..., stack reserve N KB`; the current build leaves about 56 KB).
- **Performance**: the 3D view runs Build's column visibility at half horizontal resolution
  (160 columns, `REND_XSHIFT` in `render.c`) and emits merged multi-column quads; E1L1 runs at
  about 20–30 fps in PCSX-Redux.
- **Character type**: plain `char` is unsigned, as in the Watcom-built original. The build
  applies `-funsigned-char` to the game sources.

## Status

Verified in PCSX-Redux (OpenBIOS) on the release build: boot → 3D Realms logo → title → menu →
New Game (3 episodes, 4 skills) → E1L1 with status bar, weapon, automap, doors, pickups,
level exit to E1L2; sound effects and music; in-game menu; save to memory card 1 and load back
(cross-boot), including missing/full card messages. E1L1 runs at about 20–30 fps.

Known limitations:
- Not yet tested on real hardware.
- Mirrors render as plain walls; ANM cutscenes (logo/episode intros) are skipped; no multiplayer,
  demos or RTS remote sounds.
- Wall textures fall back to half-resolution copies under VRAM pressure; world sprites are always
  half resolution (blocky at point-blank range).
- Boot to the logo takes ~13 s and a level load ~9 s at 2x CD speed (loading screens with progress
  bars); saving takes several seconds (BIOS card routines; music pauses meanwhile).
- Music is played by an SPU sequencer, not a General MIDI wavetable. The instrument samples come from
  the built-in synthesised bank unless a SoundFont is given explicitly (`DUKE_SF2`, see Building; the
  SoundFont's samples are then stored in `DUKE.DAT`). Following audiolib's EMIDI handling for a General
  MIDI device, tracks designated for other sound cards (CC110/CC111 track designation) are not played;
  `mkmusic.py` lists the dropped tracks per song when `DUKE.DAT` is built.
- The 3D column engine runs at 160 columns (`REND_XSHIFT=1`) for speed.
