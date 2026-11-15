# Changelog

## 1.0.3 (2026-11-15)
- New default keys: F9 save, F10 / F11 previous / next slot, F12 load.
- `savestate.ini` takes key names (`SaveKey=F9`, `LoadKey=Insert`, `Mouse4`, ...) as well as key codes, and the README
  explains how to change the keys. `savestate.log` lists the keys in use, and a name it doesn't know.
- If you keep the `savestate.ini` of an earlier version, it still sets the old keys (F5 / F9 / F6 / F7): replace it
  with the new one, or change the keys in it.

## 1.0.2 (2026-11-15)
- Fixed the game crashing when it switches from fullscreen to a window (or changes resolution) after a savestate was
  made: savestates now let go of the game's back buffer right before the switch replaces it.
- Such a switch deletes the savestates made before it, with a message: loading one into the rebuilt picture made the
  next switch fail. Savestates made after the switch work as usual; Alt+Tab out of fullscreen and back keeps them.
- The log records each display mode switch and the game's views of its back buffer.

## 1.0.1 (2026-11-12)
- Fixed ERR_SYS_THREAD at start-up on CPUs with many threads: the game now starts the way Project 1.27's Launch button
  starts it, with P127's settings: CPU limit to the first 4-16 threads ("core fix"), language, crash/stutter/audio fix
  flags, your own arguments, and high priority. Each can be overridden in `savestate.ini`.
- Game threads get the stack size they ask for (at least 1 MB).
- The log records the system (Windows build, CPU threads and the game's share, RAM, page file) and each game thread.
- A savestate that can't be made for lack of RAM/page file fails with a message instead of ending the game.
## 1.0.0 (2026-11-12)
First release, for GTA V 1.27 (GTA5.exe 1.0.372.2, Project 1.27).

- Full savestates within a game session: F5 save, F9 load, F6/F7 slot (10 slots), anywhere, including missions and
  cutscenes. Saving and loading each stop the game for about 0.3-0.7 s.
- Savestates last until the game is closed: the next start deletes the previous session's savestate files. A second
  copy of the game started while one runs leaves the running game's savestates alone.
- Graphics: the game's Direct3D objects are kept alive for each savestate, and the contents the game changes in place
  (such as the text glyph cache) are copied with it, so text and other GPU data are right after a load.
- Video memory: each save logs how much video memory the game uses against its budget, and how much only older
  savestates keep alive; when it runs short, the save message says so.
- Separate launcher (`GTA5-Savestates.exe`); no game file is changed or added.
