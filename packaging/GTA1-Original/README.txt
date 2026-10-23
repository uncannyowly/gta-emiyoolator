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
  A       on foot: attack; in a car: accelerate
  B       on foot: original brake action; in a car: brake / reverse
  Y       enter/exit vehicle, menu select
  X       on foot: special; in a car: handbrake
  L2      on foot: pause (P); in a car: special (horn / RC action)
  L1/R1   previous/next weapon (the original game selects the car radio)
  START   original pause / level-info screen (F6); Enter in the front end
  SELECT  city overview; press SELECT again to close
  MENU + SELECT   Esc (back / quit confirmation)
  MENU alone is reserved for Onion. Y selects menu items.
  car_buttons = 0 restores the older car button mapping.
  Holding Up and A together keeps accelerating until both are released.

Sound and music play through gtaaudio (the Mini speaker / headphones, at
OnionOS's logarithmic volume setting, including mute). As in the original, the menus have no music, only their
sounds: START/Y, Left/Right on the character, and a tyre squeal on
MENU + SELECT (Esc). Music is the car radio.
Not yet: the intro movie.
Logs after each run: launch.log and gtashell.log in this folder.

Version 0.4.0 fixes the overly quiet startup volume and skipped samples in
mono/8-bit WAV streams. launch.log now includes non-silent playback time
and the PCM peak to help diagnose missing sound.

PROGRESS AND MAP (0.4.1)
The original game records completed-stage progress, not individual missions
or your current position. Finish the stage's score objective and its ending;
use the same character next time to access the stages you have unlocked.
START shows the original pause screen and target score; it has no manual
Save option. Player-profile writes are synced to the card and logged.
The city overview is generated from your CMP map, with north at the top.
Opening it pauses play; closing it preserves an already paused game.
On a desktop keyboard, M opens/closes the overview and F6 pauses/resumes.
