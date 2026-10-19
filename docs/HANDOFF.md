# Handoff: GTA-eMiYoolator

Last updated: 2026-10-19. Keep this file current; it is the notes another agent
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
from them ("bring your own exe", like ../gta1-miyoo). Commit email is the
GitHub noreply address (set in this repo's config). The user creates GitHub
repos on the web; until then this repo is local only.

## Where things are

| What | Where |
|---|---|
| This repo | `/mnt/data/git/MiYoo-Mini/gta-emiyoolator` (local, branch `master`) |
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
scripts/build-shell.sh      # gtashell (32-bit x86, Debian i386 container) -> build/gtashell
./build/gtashell build/run/desktop.ini        # run on the PC (video = sdl2 for a window)
scripts/deploy-sd.sh /run/media/$USER/E010-96EB "/path/to/Grand Theft Auto"
```

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
  0x140–0x145 joystick axes) — loader at 0x46e820, fatal "Please run
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

## Status (0.1.0, 2026-10-19)

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
      with code 0") — harmless so far, look at it (timer thread join or fb restore).
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
- [ ] Sound and music (Miles samples/streams are silent stubs: samples report
      done at once, streams "play" until closed). Plan: own mixer in gtashell +
      SDL 1.2 audio (box86 wraps the card's native `miyoo/lib/libSDL-1.2.so.0`).
- [ ] Intro movie (Smacker: SmackOpen returns NULL, the game skips it).
- [ ] Speed tuning if needed: the frame limiter (0x47dc80) busy-waits a whole
      core; patch it to sleep. Present converts RGB565→XRGB with rotation in
      emulated code (~307k pixels per frame).

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
- Testing: `build/run/*.ini` (desktop, headless, frame dumps, scripted keys
  in `enter.txt`), `build/armrun/arm.ini` for box86 under
  `docker run --platform linux/arm/v7 ... ./build/box86/box86 ./build/gtashell ...`.
  Use a copy of GTADATA (`build/game`): the game writes PLAYER_A.DAT.
