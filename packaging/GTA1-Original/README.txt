GTA-eMiYoolator - the original Grand Theft Auto (1997) on the Miyoo Mini

This runs the game's own Windows program (WINO/Grand Theft Auto.exe from the
2002 Rockstar freeware release) through gtashell, which supplies the Windows
functions it needs, with box86 translating its x86 code for the Mini's ARM CPU.
Everything you see is the original game.

Needs, in this folder: WINO/Grand Theft Auto.exe and GTADATA (your own copy of
the game). Music is read from ../GTA1/Music (see gtashell.ini).

Buttons (change them in gtashell.ini):
  D-pad   on foot: walk the way you press (8 directions); in a car: steer,
          accelerate, brake/reverse (direct = 0 in gtashell.ini restores the
          original "rotate" controls on foot)
  A       attack                B   brake / handbrake
  Y       enter/exit vehicle, menu select
  X       special               L1/R1  previous/next weapon
  START   Enter (menus)         SELECT Esc (back, quit menu)
  MENU + START held: quit to OnionOS

Sound and music play through gtaaudio (the Mini speaker / headphones, at
OnionOS's volume). Not yet: the intro movie.
Logs after each run: launch.log and gtashell.log in this folder.
