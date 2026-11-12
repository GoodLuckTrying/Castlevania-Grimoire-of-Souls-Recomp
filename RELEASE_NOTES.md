# GoSRecomp 1.1.0

- **Pink screen in Chapter IX (levels 2-2 to 2-5) fixed** (#33, #37). The
  darkness of those levels, the one the lamps push back around your
  character, is drawn by a shader the converter couldn't translate: two of its
  tables share a slot in its constants, which the Direct3D version didn't
  allow. The converter put a placeholder in its place, and that placeholder
  painted the whole screen pink. The shader is now translated properly.
- Any shader that still can't be translated now draws nothing instead of
  pink.
- Includes everything up to 1.0.9: T-pose fix, 60 fps by default, controller
  fixes, upside-down effects.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.1.0.zip` below (not "Source code").

## A word from me

This is my last release for a while. GoSRecomp started as a personal project,
something I made to play the game myself, and I shared it because I figured
others might get some use out of it too. I did the best I could with the time
I had. I know it's not perfect and some things are still broken. Sorry about
that.

The source code is all here. If someone more skilled wants to pick it up, fix
what's left or make an even better port, please do, I'd love to see it.

If you report a bug anyway, start the game with `Play with crash log.bat` and
attach the `crash_report.zip`. That helps whoever looks at it next.

Thanks to everyone who played, tested and reported things. And let's hope
Bloodstained 2 is as good as the first one!

Known issues still open: missing or invisible effects in some places (#20,
#28, #29, #4), attacks dealing 1 damage in Chapter IV for some players (#34),
videos that Windows/Proton can't decode (#15), some button icons (#6, #7).

# GoSRecomp 1.0.9

- **T-pose in the hub and roster fixed at the source.** The loader of each
  out-of-battle character released its animation bundles when the garbage
  collector cleaned it up, even while the character was still on screen. When
  nothing else held them, the animations were unloaded and the character fell
  back to its T-pose for the rest of the session. That's why it came and went
  with launches, drivers, Proton versions or after a Dispatch mission. The
  bundles now stay loaded. The museum's enemy models had the same problem.
- Includes 1.0.8: the game runs at 60 fps by default (attacks that miss,
  invisible enemy attacks, Maria's walk).

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.9.zip` below (not "Source code").

# GoSRecomp 1.0.8

- **The game runs at 60 fps by default**: on 75/144/165 Hz monitors its own
  vsync ran it faster, which made attacks miss (#21), enemy attacks invisible
  (#26) and Maria's walk loop (#10). Vsync now stays on only where it gives
  60 fps (60/120/180/240 Hz). `fps` and `vsync` in `gosr.ini` still override it.
- Undecodable videos are now named in the crash log.
- Troubleshooting updated (videos, language, Proton versions).

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.8.zip` below (not "Source code").

# GoSRecomp 1.0.7

- Controllers other than PlayStation and Xbox (Switch Pro, 8BitDo and others
  in Switch mode) are read through their standard buttons, sticks and d-pad,
  so movement, buttons and the d-pad work without a special table. Set
  `standard=0` under `[controller]` in `gosr.ini` to turn it off, or `2` to
  use it for every controller.
- Effects upside down in the results, rewards and Develop Abilities screens
  are fixed (from 1.0.6).
- Xbox controllers: LT/RT, d-pad in menus and button icons (from 1.0.3).
- The d-pad moves your character by default.
- Troubleshooting steps in README.txt.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.7.zip` below (not "Source code").

# GoSRecomp 1.0.6

- Effects upside down in the results, rewards and Develop Abilities screens:
  fixed at the source. The game picks each effect camera's orientation per
  graphics API, and the Direct3D values didn't match how this port renders;
  it now uses the macOS values, camera by camera, like the original. The tap
  effect stays as it was.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.6.zip` below (not "Source code").

# GoSRecomp 1.0.5

- The upside-down effects fix from 1.0.2 is off again: it fixed some screens
  but flipped effects that were right (the tap effect). It stays available as
  `flip_fix=1` under `[effects]` in `gosr.ini` while a proper fix is worked out.
- The effects log describes each camera that draws effects, to find which
  ones come out upside down.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.5.zip` below (not "Source code").

# GoSRecomp 1.0.4

- More information in the crash log about the player characters in the hub
  and roster (their animation state), to track down the T-pose some players
  get there. No other change.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.4.zip` below (not "Source code").

# GoSRecomp 1.0.3

- Xbox controllers are now recognized as gamepads by the game: LT/RT, the
  d-pad in menus and the button icons ("Button11" hints) should all work. If
  your Xbox controller misbehaves, set `xinput=0` under `[controller]` in
  `gosr.ini` and report it.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.3.zip` below (not "Source code").

# GoSRecomp 1.0.2

- Effects drawn upside down in some screens (results, rewards, Develop
  Abilities) are drawn the right way up. If an effect looks wrong now, set
  `flip_fix=0` under `[effects]` in `gosr.ini` and report it.
- The d-pad now moves your character by default (`dpad_move=0` under
  `[controller]` turns it off).

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.2.zip` below (not "Source code").

# GoSRecomp 1.0.1

- Xbox controllers: LT and RT work again (dodge), in game and when rebinding.
  The d-pad on Xbox pads reads correctly too.
- New option: `dpad_move=1` under `[controller]` in `gosr.ini` lets the d-pad
  move your character.
- The log now says whether the game found your controller through Unity's
  Input System, to help with controller reports.

Update: extract into a new folder and run `Setup.bat` again, your save is kept.
Download `GoSRecomp-1.0.1.zip` below (not "Source code").

# GoSRecomp 1.0.0

First release. Castlevania: Grimoire of Souls running natively on Windows.

**You need your own copy of the macOS app, version 1.4.2, as a `.dmg`.** There's
no game data in here; `Setup.bat` takes it from your image.

## Installing

1. Download `GoSRecomp-1.0.0.zip` below (not "Source code") and extract it
   into a new, empty folder.
2. Drag your `.dmg` onto `Setup.bat`.
3. Start the game with `Play.bat`.

## What's in it

- Fully offline, no account
- Direct3D 11 with the post-processing and the particle effects
- Xbox and PlayStation controllers, keyboard and mouse (mouse buttons can be bound)
- Frame rate cap, V-Sync, window mode and resolution in `gosr.ini`
- Every language of the game
- Debug menu on F8 (it changes your save for good, back it up first)

## Known issues

- An effect can show up a few frames late the first time you see it.
- The game was made for 60 fps.

Bugs: start with `Play with crash log.bat` and attach the `crash_report.zip` it
makes to an issue.
