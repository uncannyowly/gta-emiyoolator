# Handoff: GTA-eMiYoolator

Last updated: 2026-10-23. Keep this file current; it is the notes another agent
continues from.

## Goal (the user's words and decision)

Run the **original** `Grand Theft Auto.exe` (GTA1 freeware 2002 Windows build,
`WINO` folder) on the Miyoo Mini Plus (OnionOS) so gameplay is exactly the PC
game's, instead of the clean-room engine in `../gta1-miyoo` that kept diverging.
The user approved **option 1** on 2026-10-19: a "shell" that loads the exe and
supplies the Windows functions it calls, with the x86 code run by **box86**.
If option 1 is not playable, **option 2** is static recompilation of the exe
to ARM (M-HT "SR" style), reusing the same Windows-replacement layer.
`GTA Settings.exe` is replaced, not emulated (see "Settings").

Rules: never commit the user's exe/DLLs/GTADATA/music or anything translated
from them ("bring your own exe", like ../gta1-miyoo). That includes crash
dumps: `*.core` files hold the game's memory (two were committed in 0.1.0 by
mistake; .gitignore now excludes them). Commit email is the GitHub noreply
address (set in this repo's config). The repo is public on GitHub
(uncannyowly/gta-emiyoolator, branch `master`); see "Releases".

## Where things are

| What | Where |
|---|---|
| This repo | `/mnt/data/git/MiYoo-Mini/gta-emiyoolator` (branch `master`, pushed over SSH to github.com/uncannyowly/gta-emiyoolator) |
| The clean-room port (kept, parked) | `/mnt/data/git/MiYoo-Mini/gta1-miyoo` |
| User's GTA1 install | `/mnt/scratch/Games_Wine/drive_c/Games/grand-theft-auto/drive_c/Program Files/Rockstar Games/Grand Theft Auto` (`WINO/Grand Theft Auto.exe`, `GTADATA`, `Music`, `GTA Settings.exe`) |
| SD card | `/run/media/shoots/E010-96EB` (OnionOS 4.4.0 beta); GTADATA + Music already at `Roms/PORTS/Games/GTA1/` (57 MB + 312 MB) |
| Device libs on the card | `miyoo/lib` has native ARM `libSDL-1.2.so.0`, `libSDL_mixer-1.2.so.0`, `libasound.so.2` (box86 can wrap these natively) |
| box86 source | `build/box86-src` (ignored), pinned commit in `scripts/build-box86.sh` |

## Build

```sh
scripts/build-box86.sh      # box86 for the Mini -> build/box86/box86 (needs ../gta1-miyoo's toolchain image)
```

box86 builds with `-DRPI2=1 -DARM_DYNAREC=ON` (Cortex-A7, NEON VFPv4, hard
float) against the Mini toolchain's glibc 2.28.

```sh
scripts/build-audio.sh      # gtaaudio (native ARM MI_AO player) -> build/gtaaudio
scripts/build-shell.sh      # gtashell (32-bit x86, Debian i386 container) -> build/gtashell
./build/gtashell build/run/desktop.ini        # run on the PC (video = sdl2 for a window)
scripts/deploy-sd.sh /run/media/$USER/E010-96EB "/path/to/Grand Theft Auto"
```

## Releases

v0.4.1 (2026-10-23) is the first release, asset `gta-emiyoolator-0.4.1.zip`:
extract to the SD card root (only `Roms/` at the top); players copy WINO,
GTADATA and Music from their own install into `Roms/PORTS/Games/GTA1-Original`.
The zip holds box86 (stripped, with `box86-LICENSE.txt`), gtashell, gtaaudio,
launch.sh, gtashell.ini, README.txt, VERSION, LICENSE.txt, the Ports shortcut
(`Shortcuts/Action`) and box art (`packaging/Imgs`, cropped from
`docs/screenshot.png`, so no Rockstar artwork). The packaged gtashell.ini reads
`Music` next to the game; deploy-sd.sh points a new ini at the GTA1 port's music
instead when the card has it (not copied again).
Steps: bump `VERSION`, build (box86, audio, shell), `scripts/test-shell.sh`,
`scripts/package.sh`, check the zip (no game files; binaries identical to
`build/`), commit, annotated tag `vX.Y.Z`, push, then create the release and
upload the zip through the GitHub REST API (no gh CLI here). Release notes:
what it is, what you need, install/update/remove, controls, known issues,
tested hardware, the zip's SHA-256. The 0.4.1 binaries rebuilt byte-identical
from the committed source and match the ones installed on the card.

## What the exe needs (recon, 2026-10-19)

- `Grand Theft Auto.exe`: 774 KB, `.text` 677 KB, image base 0x400000,
  SizeOfImage 0x392000, **no relocations** (must map at 0x400000), no TLS,
  entry 0x49dc30 (MSVC WinMainCRTStartup), GUI subsystem. Single-threaded
  (no CreateThread import).
- Imports: KERNEL32 63 (mostly CRT plumbing: heap, files, locale, env),
  USER32 29 (window, messages, GetKeyState, LoadStringA, MessageBoxA),
  GDI32 20 (palette/BitBlt; MGL GDI fallback), ADVAPI32 4 (registry),
  WINMM 2 (joystick), MSS32 38 (Miles: samples, streams, timers, ms clock),
  SMACKW32 8 ordinals (Smacker video), DPLAYX 2 ordinals (network play).
  `LoadLibraryA`/`GetProcAddress` bring in `ddraw.dll` (`DirectDrawCreate`,
  `DirectDrawCreateClipper`); also tries `WDIR32.DLL` (SciTech WinDirect,
  `_WD_*`) and `wing32.dll`.
- Graphics library: **SciTech MGL** is linked into the exe ("DDRAW8.DRV" …
  "DDRAW32.DRV", "3DFX.DRV" are MGL driver names, not files). Returning NULL
  for WDIR32/wing32 should leave MGL on DirectDraw.
- Settings: reads `HKLM\SOFTWARE\DMA Design\Grand Theft Auto` value
  `Language` (DWORD, 0 = English) and `...\Controls` values `Control 0`…
  `Control 9` (DWORD DirectInput scan codes; ≥0x100 joystick buttons,
  0x140-0x145 joystick axes); loader at 0x46e820, fatal "Please run
  'Settings' first" if missing. Order: Turn Left, Turn Right, Forwards,
  Backwards, Brake, Enter/Exit Vehicle, Attack, Next Weapon, Previous Weapon,
  Special. Defaults from `GTA Settings.exe`: 0xCB 0xCD 0xC8 0xD0 0x39 0x1C
  0x1D 0x2D 0x2C 0x0F (arrows, Space, Enter, Ctrl, X, Z, Tab). The only other
  registry use toggles bit 0x20 of HKCU Explorer policy `NoDriveTypeAutoRun`.
- Timing: one Miles timer at **70 Hz**, callback 0x47dc70 increments
  [0x775580]; the frame limiter 0x47dc80 **busy-waits on that counter with no
  API call**, so the timer must run on its own thread (as on Windows).
  Other waits spin on `AIL_ms_count`.
- Paths: music `..//music//trackN.wav`, data `..\gtadata\...` relative to the
  exe folder (`WINO`).

## Shell design (option 1)

A 32-bit x86 Linux program (`gtashell`) built in a Debian bookworm i386
container (glibc 2.36: no symbols newer than box86 wraps), linked only to
libc/libm/libdl/libpthread. On the PC it runs natively (fast debugging with
gdb); on the Mini it runs under box86, which runs libc natively.

- PE loader maps the exe at 0x400000, resolves imports to our `__stdcall`
  functions (unknown imports get a logging stub), sets up a Windows TEB in FS
  (via `modify_ldt`, which box86 emulates for Wine) and the Windows x87
  control word (0x27F), then calls the entry point.
- Code is compiled with `-mincoming-stack-boundary=2` (Windows code only keeps
  4-byte stack alignment, like Wine).
- DirectDraw is implemented as COM objects with system-memory surfaces;
  Flip/Unlock of the primary presents the frame.
- Presentation backends: Miyoo `/dev/fb0` (180° rotated, like ../gta1-miyoo's
  platform_miyoo.c), desktop SDL2 via `dlopen` (no link dependency), and
  PPM frame dumps for headless tests.
- Input: Miyoo evdev buttons / desktop keys → WM_KEYDOWN/WM_KEYUP with PC
  scan codes in lParam, matching the registry controls above.
- Miles: timers on a pthread; samples/streams first as silent stubs that
  report "done", real audio later.
- Settings: values come from `gtashell.ini` (replaces `GTA Settings.exe`).

## Status (0.3.1, 2026-10-19)

- [x] Recon of the exe (above).
- [x] box86 cross-built for the Mini (`scripts/build-box86.sh`, stripped on deploy, 6.5 MB).
- [x] gtashell runs the original exe on the PC: logos/menu, city select (Liberty
      City, "1. Gangsta Bang"), gameplay with the original pager brief, at the
      game's own ~23 fps (70 Hz timer / 3).
- [x] The same binaries run under box86 on ARM (Docker arm/v7 emulation): full
      gameplay, 23 fps steady for 5 minutes.
- [x] Installed on the SD card (`scripts/deploy-sd.sh`): `Roms/PORTS/Games/GTA1-Original`
      + shortcut `Roms/PORTS/Shortcuts/Action/Grand Theft Auto (Original).port`
      + box art. Own GTADATA copy (the game writes PLAYER_A.DAT); music read
      from `../GTA1/Music`. The GTA1 port's files and save were not touched.
- [x] **First run on the device (0.1.0, 2026-10-19):** user: "absolutely
      playable and looks fine and plays fine". Log: steady 23.3 fps (the
      original's pace), 72 MB available before start, swap untouched. Box86
      segfaults *after* the game exits cleanly (exit code 139 after "exe exited
      with code 0"): harmless so far, look at it (timer thread join or fb restore).
      Device logs are copied to ignored `build/hw-logs/`.
- [x] **Direct 8-way walking (0.2.0)**, user request ("so I don't have to
      constantly hold UP and DOWN"): `src/input.c` `steer()`. On foot in a level
      the D-pad is a direction; the shell reads the facing and holds the game's
      own Turn Left/Right (registry controls 0/1) until within 24 units, and
      Forwards (control 2) while within ±128. Game memory used: local player
      index `[0x74f83c]`, player record `0x74f148 + 444*i` (+2 ped id, +0xbc
      2 = on foot / 0 = in car), peds `0x7284e0 + 256*id` (+0xa8 facing 0..1023,
      0 south, 256 east, Turn Right decreases it ~48 per frame; +0x90/+0x94
      position, 16.16 with 64 units per block in the high word). "In a level" =
      DirectDraw primary has a flip chain (`ddraw_in_level`). `watch = 1|2` in
      the ini logs these fields for further research.
- [x] **Sound and music (0.3.0)**, pending a hardware listen. `src/mss32.c`
      implements Miles samples (game memory, format/rate/volume/pan/loops) and
      streams (TrackN.wav: pause, volume, loops, byte positions the radio uses
      to resume) and mixes 22050 Hz stereo on a thread. Output is a pipe into
      `audio_cmd`: on the Mini the native helper `build/gtaaudio`
      (`tools/gtaaudio.c`, `scripts/build-audio.sh`, MI_AO code from
      ../gta1-miyoo; its stderr lands in launch.log). launch.sh kills the
      audioserver. Desktop check: `audio_wav = file` records the mix; a
      cross-correlation against the tracks found the car radio's Track2 at
      61 s (0.61 vs ~0.19 others), i.e. right speed and format. box86 fork/exec
      of the helper verified under ARM emulation.
- [x] Frame limiter patched to sleep (`patch_spin` in pe.c, `patch_spin = 0`
      disables): CPU 16 s → 3.2 s per 25 s of play on the PC, same 23.3 fps.
- [x] **Level-start crash fixed (0.3.1)**, user report on 0.3.0: "crashes
      immediately post character select". Not the sound code: the 0.1.0 session
      had returned to the front end, which rewrites `PLAYER_A.DAT` with screen
      mode 0; the next launch sets mode 6 (see below) and the level start reads
      the missing table entry (SIGSEGV at 0x414ce3). `patch_mode` in pe.c.
      Verified natively and under box86 (ARM emulation) with the card's save,
      and play → Esc, Enter to the front end → relaunch → level.
- [ ] Sound on the device, 0.3.0 report: "no sounds on the title screen unless
      I exit, then I hear the tire squelch". That matches the original (see
      "Front-end sound" below): the squeal is the game's Esc sound, sent by MENU
      before MENU + START quits. The box86 (ARM) mix equals the PC mix. In-game
      sound and music still need a listen; 0.3.1 gtaaudio logs the driver's
      volume/mute and the seconds played to launch.log.
- [ ] Intro movie (not wanted by the user).
- [ ] Exit segfault under box86 after a clean exit.

## Things learned the hard way (keep)

- MGL (inside the exe) always loads **WDIR32.DLL** (SciTech WinDirect) and
  resolves ~45 `_WD_*`/`_PM_*`/`_VF_*` names with GetProcAddress (cdecl):
  `src/wdir.c` stubs them; `PM_allocRealSeg` must return a buffer.
- MGL checks `FindFirstFileA("C:\WINDOWS\SYSTEM\ddraw.dll")` before using DirectDraw.
- MGL looks up `CreateDIBSection`/`SetDIBColorTable` in gdi32 at run time.
- MGL asks for a primary with **7 back buffers** and walks the chain with
  EnumAttachedSurfaces (first must carry DDSCAPS_BACKBUFFER) until its count:
  a short chain loops forever.
- The game calls AIL_shutdown/AIL_startup between front end and levels; the
  70 Hz timer registered later must still run (the frame limiter busy-waits
  on its counter).
- box86 `modify_ldt` only emulates op 0x11 with entries 7..25 or ≥ 0x1ff0:
  we use entry 0x1ff0 (selector 0xff87).
- Build with `-D_FILE_OFFSET_BITS=64`: under box86 the 32-bit `readdir`
  returns nothing (case-insensitive file lookup broke).
- Screen modes: the game keeps only 15/16/32 bpp 4:3 or 16:10 modes ≤ 1600
  wide from DirectDraw (table at [0x504cd0], 28-byte entries; counts per depth
  at 0x503204..0x50320c), so ours is 640x480x16, 640x480x32. Current mode byte
  [0x503224]; PLAYER_A.DAT byte 4 stores it (0x4274ff) but loading ignores it:
  0x414c70 sets 6 if it is below the count, else 0.
- Front-end sound (LEVEL000.SDT, 16 samples; traced on the PC): Enter = sample
  2 (stereo burst), Left/Right on character select = 3 (clicks), Esc = 4 (tyre
  squeal), Up/Down nothing. No music on the title or menus: `..\GTADATA\AUDIO\N.WAV`
  jingles (0x404e00) play only in menu state 1, after a level. Menu states
  (0x427030): 7 main menu, 11 character, 8 city map, 6 start level.
- Testing: `build/run/*.ini` (desktop, headless, frame dumps, scripted keys
  in `enter.txt`), `build/armrun/arm.ini` for box86 under
  `docker run --platform linux/arm/v7 ... ./build/box86/box86 ./build/gtashell ...`.
  Use a copy of GTADATA (`build/game`): the game writes PLAYER_A.DAT.
  `debian:bookworm-slim` with `--platform linux/arm/v7` works as the ARM
  image; `audio_wav` records the mix there too. gdb on the PC: break on
  `patch_exe` first, then on exe addresses (the exe is mapped by then).

## Continuation: 0.4.0 (2026-10-19)

- Read the plugged-in card's latest 0.3.1 logs (16:21-16:27): clean game
  return, audio helper played 381.2 s, startup volume **-51 dB**, no mute
  reported. The box86 exit segfault still follows the clean game exit.
- **Sound volume fix:** helper incorrectly used `vol * 3 - 60`; Onion uses
  `round(48 * log10(1 + vol)) - 60` (volume 0 is -60). At vol=3 this is
  -31 dB, versus our -51. Reference: OnionUI/Onion
  `src/common/system/volume.h`. Prefer `/mnt/SDCARD/system.json`, with
  `/appconfigs/system.json` fallback; honor `.tmp_update/config/.muteVolume`
  and set MI_AO mute explicitly. Leave live volume adjustments to keymon.
- **WAV fix:** stream_refill read 8192 bytes but decoded only 2048 frames;
  mono/8-bit input lost up to 3/4 of its data. Limit each read to 2048 source
  frames, and clear fractional resampling position on seek.
- Native helper retries interrupted reads, bounds repeated SendFrame failures
  (logs and drains at real-time pace rather than hanging forever), and logs
  non-silent seconds plus PCM peak at exit. Neither the volume fix nor the
  buffer fix has yet been confirmed by listening on the actual Mini.
- **Contextual buttons:** `car_buttons = 1` (default) makes A forwards,
  B backwards/brake/reverse, X brake/handbrake, L2 Special (horn/RC action)
  while the player's mode is 0. Walking and menus keep existing mappings;
  the D-pad retains direct walking and conventional car steering. Y remains
  enter/exit. This matches the other port's core driving layout. L1/R1 remain
  stock weapon actions; no custom radio-station or bomb behavior is added.
  `<button>_car` overrides are supported; `car_buttons = 0` is the fallback.
- Input now combines physical sources before sending key edges, fixing
  Up+A throttle and Y+Start aliases and releasing keys across mode changes.
- Tests: `scripts/test-shell.sh` (32-bit; use i386 toolchain Docker when the
  host sandbox rejects 32-bit syscalls). Covers synthetic game memory,
  walking/car/menu transitions, button aliases, legacy layout, and continuous
  5000-frame mono/stereo 8/16-bit streams across refill boundaries.
  `tests/volume.c` tests the helper's Onion curve; compile with MI SDK headers
  and `-ldl -lm`. SDK headers can be copied from the ARM toolchain to ignored
  `build/tests/sdk`, then tested with host gcc.
- The current workspace is `/mnt/data/git`, not `/home/shoots/git`.
- Verification completed: control tests, all four WAV format regressions,
  volume-curve/clamping test, and helper no-device fallback. Native headless
  gameplay reached the car; recorded PCM contains menu effects and substantial
  in-game audio (14-26 s RMS 8557/32768). ARM/box86 also reached gameplay and
  successfully spawned the native helper (no physical MI_AO in emulation).
  These smoke runs are time-limited; they do not validate the device speaker
  or resolve the already-known box86 exit crash.
- Optional full test command once public SDK headers are in build/tests/sdk:
  `MI_SDK_INCLUDE=build/tests/sdk scripts/test-shell.sh`.
- **Installed 0.4.0 on the plugged-in SD card**, with SHA-256 verification
  of shell, ARM helper, README, VERSION and both configuration files; synced.
  The active ini retains existing settings and gains car_buttons=1. Previous
  ini is `gtashell.ini.before-0.4.0`; previous binaries/logs are also saved in
  ignored `build/hw-logs/0.3.1-latest`. No game data, saves or other ports were
  changed. Next: user listen/control check on the actual Mini; inspect the
  new launch.log volume/mute, non-silent duration and PCM peak if needed.

## Continuation: 0.4.1 (2026-10-19)

User requested MENU/Onion + SELECT = Esc, SELECT = map, START = game menu.
When asked about saving, user explicitly chose **original completed-stage
progress**, not mid-mission save states. Do not add a manual save-state system.

- `menu_controls=1`: START sends F6 during a level and Enter in the front end;
  SELECT opens/closes the new city overview; MENU alone sends nothing to the
  game. MENU+SELECT sends only Esc, in either press order, latched until both
  release. SELECT acts on release to avoid accidental map toggles from the
  chord. Removed the old MENU+START quit action. Y still confirms menus.
- F6 is the original pause / target-score screen, not a custom Save menu.
  See the original manual: https://gtamp.com/GTA1/gta1manual.pdf (pages 7-11).
  Original stage-completion progress is automatic and character-specific;
  individual missions/current location are not saved by this executable.
- `src/mapview.c` reads the user's current CMP (observed via CreateFileA),
  builds a north-up street/water/building/park overview, and replaces presented
  frames only while open. No map assets or game-data derivatives are committed.
  This is a static city overview, with no player marker or waypoint routing.
  It uses F6 while open and leaves an already paused game paused. Native pause
  flag is byte `[0x7846b8]` (0x481510 / 0x481530), not the UI word 0x513240.
  Desktop/script map shortcut is M (`map_key=0x32`). Car and walking layouts
  from 0.4.0 stay intact.
- Saving durability: FlushFileBuffers now really fsyncs instead of returning
  success without flushing. Dirty file handles sync on close; player-profile
  writes report bytes/sync success to gtashell.log. This does not change when
  the original game decides a stage has been completed.
- Tests cover both chord orders/release orders, map suppression of gameplay
  keys, START in and out of a level, map format bounds/truncated input,
  preservation of native pause state, and profile write/readback/flush/close.
  Existing sound and volume tests also pass.
- Native gameplay test: overview visually inspected, original pause/target
  screen inspected, return to front end wrote and synced a 1044-byte copied
  profile at 30.106 s. This is not a full natural stage-completion test.
  Fixtures/logs/screenshots are ignored under build/verify-0.4.1.
- A diagnostic run with audio=0 stalled in the original game's wait for a
  sample to finish; retest with audio=1 and empty audio_cmd for silent output.
  The supported device config continues to use audio=1 and the native helper.
- ARM/box86 0.4.1 verification: map opened at 16.170 s and closed at 20.156 s
  with the real game's pause flag confirmed set; resumed at 23.3 fps. Native
  helper spawned successfully. This smoke run ended at its 25 s timeout.
- Installed and SHA-256 verified 0.4.1 on the SD card, synced. Active config
  migrated to start=0x40, select=0, menu=0, menu_controls=1, start_menu=0x1c.
  Old ini is gtashell.ini.before-0.4.1. Latest 0.4.0 logs/config/binary/profile
  backed up under ignored build/hw-logs/0.4.0-latest. The card's PLAYER_A.DAT
  hash was checked before/after installation and remained identical.
