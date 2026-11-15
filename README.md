# GTA V savestates (1.27) — version 1.0.3

Full savestates for GTA V **1.27** (GTA5.exe 1.0.372.2, as installed by Project 1.27), like snapshots of a virtual
machine. Save anywhere, including in the middle of a mission, a cutscene or a chase, and jump back to that exact
moment. Everything comes back: mission scripts and their timers, every ped and car with its AI, physics, traffic,
wanted level, weather, time of day and the camera. Ordinary save files only keep a summary of your progress and can't
be made during missions; these can.

It is a separate program: **no file of the game is changed or added**. Start the game with `GTA5-Savestates.exe` to
have savestates; start it from Project 1.27 as usual to play the untouched game.

| Key | Action |
|---|---|
| **F9** | save a savestate to the current slot |
| **F10 / F11** | previous / next slot (10 slots) |
| **F12** | load the current slot |

A short message in the top-left corner confirms each action ("Savestate 1 saved (1.02 GB, game stopped 390 ms)").
Saving and loading each stop the game for about 0.4 seconds. The keys can be changed (see
[Changing the keys](#changing-the-keys)).

**Savestates last until you close the game.** The next time the game starts with `GTA5-Savestates.exe`, the previous
session's savestates are deleted.

## Requirements
- GTA V downgraded to 1.27 with Project 1.27 (GTA5.exe 1.0.372.2), Windows 10/11, 64-bit.
- About 1 GB of disk space per savestate.
- About 1 GB of free RAM (or page file) while a savestate is made: it is collected in memory, then written. If there
  isn't enough, the save fails with a message and the game goes on.

## Video memory
A savestate keeps alive every texture and model the game had loaded when it was made, so it can bring them back.
The game frees what it no longer needs, but the textures a savestate holds stay in video memory until that slot is
saved over (or the game is closed). One or two savestates in the same part of the map cost little; many in different
places add up. On cards with 2-4 GB:
- Each save checks how much video memory the game uses against what Windows gives it. When it is nearly full and
  other savestates hold a large part of it, the save message says so (for 8 seconds) with the size.
- Then save over slots you don't need (in the same area, or after a load), or restart the game. Running over the
  budget doesn't crash the game: Windows moves textures to system RAM, and the game stutters until memory is freed.
- `savestate.log` lists the numbers for each save ("video memory ... MB in use").
- The extra GPU copies each savestate makes itself are small (about 4 MB).

## Use
1. Put the folder with `GTA5-Savestates.exe`, `savestate.dll` and `savestate.ini` anywhere.
2. Open Project 1.27 as usual (so you're signed in), but start the game by running `GTA5-Savestates.exe` instead of
   P127's Launch button. The launcher finds the game folder itself (or asks once and remembers it in
   `savestate.ini`), then starts `PlayGTAV.exe` the way Project 1.27 does, with P127's settings: language, its
   crash/stutter/audio fixes, your own arguments, the "core fix" CPU limit and high priority.
3. Play. `savestate.log` next to the launcher lists every save and load (`savestate.prev.log` is the previous game
   session's).

To uninstall, delete the folder. Nothing was installed anywhere else, and the game folder is unchanged.

## Changing the keys
1. Close the game.
2. Open `savestate.ini` (next to `GTA5-Savestates.exe`) in Notepad.
3. In the `[Savestate]` section, replace the key after the `=` on these lines:

   | Line | Action | Default |
   |---|---|---|
   | `SaveKey=` | save to the current slot | `F9` |
   | `PrevSlotKey=` | previous slot | `F10` |
   | `NextSlotKey=` | next slot | `F11` |
   | `LoadKey=` | load the current slot | `F12` |

   For example `SaveKey=Insert` and `LoadKey=Home`.
4. Save the file and start the game with `GTA5-Savestates.exe` again.

Key names (upper or lower case):

| Keys | Names |
|---|---|
| function keys | `F1` ... `F24` |
| letters and the top-row digits | `A` ... `Z`, `0` ... `9` |
| numeric keypad | `NumPad0` ... `NumPad9`, `NumPadAdd`, `NumPadSubtract`, `NumPadMultiply`, `NumPadDivide`, `NumPadDecimal` |
| editing and arrows | `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`, `Right` |
| others | `Pause`, `ScrollLock`, `Backspace`, `Tab`, `Enter`, `Space`, `Minus`, `Equals`, `Comma`, `Period`, `Semicolon`, `Slash`, `Backquote`, `LeftBracket`, `Backslash`, `RightBracket`, `Quote` |
| mouse | `MouseMiddle`, `Mouse4`, `Mouse5` (the side buttons) |

A Windows virtual-key code works too (`120` or `0x78` is F9), for keys not in the list. Leave a line empty
(`SaveKey=`) for its default.

- Pick keys the game doesn't use for something else (its key bindings are in Settings): a key does both.
- The keys only work while the game window is in front.
- `savestate.log` shows the keys in use at the start ("keys: save F9, load F12, ..."). A name it doesn't know is
  noted there, and the default is used instead.
- With the Steam version of the game, F12 also takes a Steam screenshot. Use another `LoadKey`, or change Steam's
  screenshot key, if that bothers you.

## Troubleshooting
- **ERR_SYS_THREAD at start-up**: GTA V 1.27 can't handle CPUs with many threads. The launcher limits it to the first 4-16
  CPU threads, like Project 1.27's "core fix", unless that is turned off in Project 1.27 or with `CoreFix=0` in
  `savestate.ini`. The log's "system:" line shows how many threads the game got.
- **"the game didn't stop in time"**: a game thread was busy (loading screens); try again a moment later.
- **"not enough memory (RAM and page file)"**: close other programs or enlarge the page file; nothing was saved.
- **"it was made in an earlier game session"**: savestates don't survive a restart of the game.
- **"Savestate 1 deleted: savestates don't last through a switch between fullscreen and a window..."**: see Things
  to know below.
- **Crashes or other problems**: `savestate.log` (and `savestate.prev.log` after a restart) says what happened last;
  include it when you report a problem.

The launcher starts PlayGTAV.exe paused, loads `savestate.dll` into it, and the DLL loads itself into GTA5.exe before
any of the game's code runs. Nothing is written to the game folder, and the ASI loader or ScriptHookV aren't needed.

## Things to know
- Each savestate is a file in the `savestates` folder (`slot01.gss` ... `slot10.gss`, about 1 GB each). Saving to
  a slot replaces its file. The files are deleted the next time the game starts (with the `.gpu` files and the
  `savestates\gpu` folder that earlier versions left there).
- Loading savestates in a later game session was tried, but some loads crashed the graphics driver (for example in
  the Prologue), so savestates are kept to the session they were made in. The code for it is still there behind a
  lab setting (`[Lab] OtherSessions=1`) and isn't meant for playing.
- Loading a normal save from the pause menu doesn't affect savestates: you can load a savestate made in another save
  file, before or during another mission.
- If the game can't be stopped within 5 seconds (it happens very rarely), nothing is saved or loaded and the message
  says so.
- Each savestate keeps the textures and models that were loaded when it was made (see Video memory above).
- Switching between fullscreen and a window (Alt+Enter or the Graphics settings) or changing the resolution deletes
  the savestates made before it, with a message: the game builds its picture anew then, and a savestate from before
  can't be loaded into that safely. Savestates made after the switch work as usual. Alt+Tab out of fullscreen and
  back keeps them.

- Other mods that keep their own state (trainers, scripts) are not rolled back by a load.

## How it works
GTA V keeps its state in its global variables, in a few large memory pools it reserves at start-up, and in the
stacks of its ~80 threads. A savestate stops every game thread at a safe point (inside the wait functions the game
calls, which the DLL hooks), then writes the game's global data, every page of its pools that has been written (the
pools are reserved with write watch), and each thread's stack and registers. Loading writes the memory back and
puts every thread back at the point where it was stopped, so each one continues exactly where it was.

Things outside the game's memory are handled separately:
- **Direct3D 11**: every texture, buffer, view and shader the game creates is tracked. A savestate keeps them all
  alive; a load sets their reference counts back and restores the device context's pipeline state.
- **Kernel objects**: the state of the game's events and semaphores, the owners of its mutexes, and file positions
  are saved and put back. Handles the game closes stay open while savestates may need them.
- **Time**: the game's clocks run behind the real ones by an offset that a load adjusts, so timers see no jump.
- **Threads**: a thread the game ends while savestates exist stays parked (to the game it has ended), so a savestate
  that needs it can bring it back.
- **Audio**: XAudio2 is paused while a savestate is made or loaded.

The DLL also puts the game's memory at the same addresses in every session and the game's handles at fixed values;
that groundwork is from the attempt to load savestates in later sessions (see `MODLOG.md`).

The engineering journal, with every finding and bug on the way, is in `MODLOG.md`.

## Building
Visual Studio Build Tools (x64) and `build.bat` produce `build\savestate.dll` and `build\GTA5-Savestates.exe` and
copy the release files to `dist\`. `build.bat test` runs the allocator stress test. `tools\labrun.ps1` drives
lab sessions in the real game (start, wait for story mode, press keys, take screenshots).

## License and credits
- MIT license (`LICENSE.txt`). GTA V is Rockstar Games'; this tool contains none of its files.
- Made with Anthropic's AI (Claude Code, Claude Opus), with direction by Crab1k. Follows the design of the GTA San
  Andreas savestates project.
- Changes per version: `CHANGELOG.md`.
