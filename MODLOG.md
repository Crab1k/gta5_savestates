# MODLOG: GTA V 1.27 full savestates

Goal (user, 2026-11-02): like `E:\Claude Projects\gta_sa_savestate`, savestates that save and load at any point,
including during missions and other activities. Game: `E:\Grand Theft Auto V`, downgraded by Project 1.27
(`H:\Project 1.27`) to GTA5.exe **1.0.372.2** ("1.27"; P127's `DowngradeFiles124` holds 1.0.323.1 = "1.24").
ScriptHookV is switched off in P127 (user choice); nothing here depends on it.

## Recon (2026-11-02)
- GTA5.exe 1.0.372.2: x64, ASLR (base 0x7FF79CDA0000 this boot), Arxan: code bytes on disk are encrypted (static
  disassembly of the file is garbage), second `.text` section. Writable sections: `.data` 0x1077928 bytes at RVA
  0x1AA2000, `BINKBSS` 0x60, `.tls`. Normal import table (dumpbin), patching IAT slots works.
- Imports of note: kernel32 `HeapAlloc/Free/ReAlloc/Size`, `GetProcessHeap` (no HeapCreate: the static CRT uses the
  process heap), `VirtualAlloc/Free`, `CreateThread`, `Sleep`, `WaitForSingleObject`, `WaitForMultipleObjects`,
  `GetOverlappedResult`, `ReadFile`, `CreateEventA`, `CreateSemaphoreA`, `CreateMutexA`, `GetWriteWatch`,
  `QueryPerformanceCounter`; winmm `timeGetTime`; ole32 `CoCreateInstance` (XAudio2 2.7, MMDeviceEnumerator);
  DSOUND by ordinal. d3d11/dxgi are loaded with LoadLibraryA + GetProcAddress (`D3D11CreateDeviceAndSwapChain`).
- Launch chain: P127 runs `cmd /c cd /d "E:\Grand Theft Auto V\" && start /affinity FFF playgtav.exe -uilanguage
  american`; PlayGTAV.exe (DragonEmu, imports `CreateProcessA`, no injection of its own) starts `GTA5.exe
  -uilanguage american`. Starting PlayGTAV.exe directly works (P127 was open, already authenticated via MTL).
- The ASI loader in the game folder (`dinput8.dll`, 2015) reports every plugin "failed to load" since P127's
  ScriptHook switch was turned off; DLLs load fine otherwise (CreateRemoteThread injection works).
- Memory (recon DLL, early): 8 VirtualAlloc reservations at start-up, all through rage's allocator at
  `GTA5.exe+115C7D5`: 1920 MB, 8 MB, 2x 256 MB, 196 MB, 205 MB (+ small), all MEM_COMMIT|MEM_RESERVE RW; a further
  256 MB when story mode loads (and at mission start, replay buffer?). Windows heap: ~160 blocks at start-up, none
  in game. Process total ~5 GB private; the rest belongs to the driver, D3D, XAudio2, DragonEmu.
- Threads: ~80 game threads, all created through `GTA5.exe+115C0C8` (rage sysIpcCreateThread) with start
  `+115C2A8`. Waits go through rage wrappers (`+115C21C`, `+115C234`, `+115C423`, `+115C43E`, `+1155DC5` task
  workers, ...); one thread spins on `Sleep(0)` (`+11172E0`). No thread names (rage only names them under a
  debugger). External threads: XAudio2 (runs the game's voice callbacks), DSound, bink2w64, d3d/driver, DragonEmu.

## Route: whole-memory snapshot plus thread checkpoint (chosen 2026-11-02)
Like the SA project (game memory + global data copied verbatim, external objects kept alive and reconciled), with one
change: GTA V has ~80 threads, so instead of finding an idle point for each one, every game thread is parked inside
our wait hooks and its stack + registers are part of the savestate. A load puts each thread's stack back and resumes
it at its saved park point (RtlCaptureContext / RtlRestoreContext; the thread copies its own stack after moving its
frame below the saved region with _alloca).
- Memory: GTA5.exe writable sections; every region GTA5.exe reserves (VirtualAlloc hook adds MEM_WRITE_WATCH so only
  written pages are saved); the Windows heap blocks of GTA5.exe redirected to a 64-bit TLSF arena (16-byte blocks).
- Per thread: CONTEXT, stack [Rsp, StackBase), implicit TLS block of GTA5.exe (TEB+0x58[_tls_index]).
- Kernel objects (hooks on CreateEventA/SemaphoreA/MutexA/CreateFile*): event state, semaphore count, mutex owner +
  recursion (NtQueryMutant class 1 = owner CLIENT_ID; ownership moved by the parked threads themselves), file
  position. Game CloseHandle of events/semaphores/mutexes is deferred (savestates may still use them).
- Time: QueryPerformanceCounter and timeGetTime of GTA5.exe run behind the real clocks by an offset; a load sets the
  offset so the game's clock continues from the saved moment.
- D3D11: device vtable hooks on every Create*; each object gets a private-data "sentinel" whose final Release says it
  was destroyed. A savestate pins all live objects (one AddRef) and records the game's count; a load sets counts
  back and re-applies the immediate context's whole pipeline state. tests/d3d11_refs.cpp: on this AMD driver views
  and pipeline bindings do not change public refcounts.
- XAudio2: StopEngine around save and load (its thread runs game callbacks).
- Overlapped disk reads/writes must have completed before the snapshot (pipes excluded: DragonEmu keeps one pending).

## Progress log
- Arena: tests/arena_test.cpp 388k random ops, 0 corruption, 16-byte aligned, coalescing OK.
- Launcher (`GTA5-Savestates.exe`): starts PlayGTAV.exe suspended, loads savestate.dll at its entry point (x64 stub:
  RCX = entry, RDX = PEB); in PlayGTAV the DLL hooks `CreateProcessA` and does the same for GTA5.exe. Game folder
  untouched.
- First save: 82 threads parked in 0 ms, 0.88 GB file (885 MB written pages), game stopped 344 ms. First load: back at
  the saved spot, stopped 360 ms.
- Crash 27 s after a load (GTA5.exe+11E85A2) after riding away: D3D objects destroyed after the save. Fixed with the
  pinning above (41k objects pinned per savestate; loads release the 1-5k created since).
- Region leak: the game reserves a new 256 MB region after loads (restored memory doesn't know the one reserved after
  the save). Loads now release regions that neither the loaded savestate nor any other slot of the session has.
- Hang (game "not responding", all threads waiting in our hooks) after a second load in the Prologue mission: the
  main thread waited on a rage **mutex** owned by another thread in the live timeline. Mutex ownership is now saved
  and restored (usually 1 mutex changes per load). Mission scenario: 6 loads alternating two slots, no hang.
- Mission test (Pause > Game > Replay Mission > Prologue): save at "Go to the guard", play into the hostage scene,
  load: same frame; walking on triggers the scripted guard takedown again (mission script continues correctly).
- Stress: 12 alternating loads of two slots in free roam, 8 ride/load cycles with re-saves: no crash.
- Audio: XAudio2 performance data before saves / 3 s after loads: 1 of 1 source voice playing, 0 glitches (rage mixes
  everything into one voice). The user confirmed by ear that audio is fine.
- Overlay: messages drawn into the back buffer in an IDXGISwapChain::Present hook (GDI text -> immutable texture,
  shaders compiled with d3dcompiler_47, pipeline state saved/restored with references held). Objects it creates are
  excluded from tracking (`d3d::OwnObjects`).
- Save stop time 1.75 s with a fresh savestates folder (writing 1 GB through a busy file cache): the snapshot is now
  copied into 64 MB memory chunks while the game is stopped and written after it runs again: 220-360 ms.
- Cross-savefile (Pause > Game > Load Game while savestates exist): first attempt refused ("a game thread it needs
  has ended": loading a story save ends 5 game threads and starts new ones). Ended threads now become zombies while
  savestates exist (parked with stack and id; waits on their handles succeed; thread handle closes held back); a load
  revives the zombies it needs and turns the threads it doesn't know into zombies. Second attempt: the savestate
  loaded, but the next load showed the game's error box `ERR_STR_INVALID_DATA_1`: the story-save load closes and
  reopens `update\x64\dlcpacks\{mpheist,mpchristmas2,mpluxe}\dlc.rpf`, so restored memory read through handle values
  reused for other files. Closes of files opened read-only and shareable are now held back while savestates exist
  (files the game writes, like saves, still close). Final run: savestate in a Franklin save, story-load a Michael
  save, savestate there, then load 1, 2, 1: all correct, no error.
- Lab notes: keybd_event needs KEYEVENTF_EXTENDEDKEY for arrows (else they arrive as numpad keys and GTA's menus
  ignore them); SetForegroundWindow from the lab script needs an Alt tap first. Replay Mission writes MISREP0000 and
  a story-save load triggers an autosave (SGTA50015) in the player's profile: the profile was backed up to
  `lab\profile_backup_<profile id>` before testing and restored byte-for-byte afterwards.

- Savestates of earlier sessions (file format 3). Groundwork, each checked in the lab:
  - Census of saved memory: what it points at outside the game (D3D objects ~170k pointers, a few dozen other DLL
    objects, DLL images, the window).
  - Handle block: 32640 handle values (0x40000-0x60000) reserved with dummy events at start-up; the game's handles are
    put at fixed values in creation order and each savestate keeps a description per handle. A forced in-session
    rebuild recreated 1458 kernel objects and the game ran on.
  - GPU manifest: creation descriptions of every game D3D object, contents of immutable and default resources in a
    content-addressed store (savestates\gpu, read back in the Present hook), shader code by hash. A forced
    in-session rebuild made 41,795 objects in 4.2 s, rewrote 175k pointers, renders correctly.
  - Fixed address layout: regions in a 64 GB window per thread number (creation order, main = 0), stacks and
    thread-local blocks in fixed slots, the heap arena at a fixed address. Two separate sessions logged identical
    layouts (103 lines).
- Load from another session, how it works: saved threads go to the live threads with the same number (missing
  ones are started and parked as zombies first); thread ids the game sees are virtualised (the saved ones); the
  game's FLS/TLS slot values are put back by each thread; critical sections (hooked Initialize/Delete, list in the
  file) get their owner tids translated, fresh wait events and no stale debug records; regions are re-reserved at
  their addresses with the saved committed runs; kernel objects that differ are made new; GPU objects are rebuilt
  from the manifest; then one pass rewrites old D3D pointers, pointers into DLLs that moved (module table in the
  file), window handles (by class) and other DLLs' objects (outside-pointer table made after each save from the
  snapshot copy: slot address, value, vtable class; the live value in the same slot, read before the restore, is
  the new one). Data sections, regions, saved stacks, TLS blocks, thread contexts and slot values all go through it.
- Bugs found on the way:
  - GPU rebuild took 75 s the first time: ~24k contents opened one file at a time, cold. Now read ahead with 12
    threads before the game is stopped (0.3-0.9 s); the rebuild itself ~4 s.
  - The process vanished during the thread restore with nothing logged; Windows' Application log said 0xC0000409
    subcode 2 in savestate.dll: our own frames on every saved stack carry /GS cookies made with the previous
    process's random cookie. The DLL is now built with /GS- (the game's cookie is in its .data, restored with it).
  - Next, a call through garbage: the game's C runtime scrambles function pointers with EncodePointer, whose secret
    is per process. GTA5.exe's EncodePointer/DecodePointer now use a fixed scrambling.
  - The exception dispatcher then crashed walking the stack: RunOnStack's unwind info used RBP, which held the old
    session's value. RunOnStack now keeps a machine frame (return address and caller RSP) at the top of the new
    stack; the load writes this thread's RSP into it (threads::WayBack).
  - Restored frames of our hooks held the old session's Thread*: Park() now returns the live record and every
    caller uses it; wait deadlines use the game's virtual clock instead of GetTickCount.
  - One thread never took its restore command: its wake event sat at a handle value inside the block (file closes
    free a block value for a moment while the dummy is put back; another thread's CreateEvent took it), and the load
    installed a game object there. Our handles now leave the block (kobj::Ours), and a load never replaces a block
    value that isn't a dummy or a game object.
  - The launcher pipe sits at another value in the next session and can't be opened again: the load duplicates the
    live session's handle to the same pipe.
  - D3D objects made by other DLLs (Social Club overlay, GPU libraries) are no longer treated as the game's (only
    creations called from GTA5.exe, and the rebuild's own); the first version of that check left rebuilt objects
    untracked, so a save made after a cross-session load pinned only 972 objects.
- Results: save in session 1 after riding away from the start, quit, start session 2 (freeway), F9: back at the
  intersection where session 1 saved (screenshots), game stopped 4.7-5.0 s. Two cross-session loads in a row, then a
  save and in-session load in session 2; a three-session chain (session 3 loads session 2's save made after its own
  cross-session load); 60 s of riding after a cross-session load: no crash, audio "1 of 1 voices, 0 glitches".
- Mid-mission across sessions: Pause > Game > Replay Mission landed in the intro cutscene (Michael at the therapist,
  opening credits running); saved there (69,349 GPU objects, 1.12 GB). Next session, in free roam on a bike, F9: back
  in the cutscene at the same credit, game stopped 8.0 s (7 s GPU rebuild, 2 mission-only regions re-reserved). The
  cutscene played on, the beach intro followed and gameplay started (driving, help text): 3 minutes, no crash.
- The GPU store now drops contents no slot manifest refers to when the game starts (first run: 327 MB removed).
- A savestate keeps savestate.dll's link time and size: our frames are on every saved stack, so a file only loads
  with the build that made it.
- User reports (2026-11-02/06):
  - A cross-session load froze the game for 4.5 min (146,283 GPU objects, 1.9 GB of contents, 2.8 GB of RAM free,
    28.9 of 33.8 GB committed): the new session's 45k objects were still alive while the old ones were rebuilt, and
    the rebuild opened ~95k store files one at a time while the system paged. A second load right after it was
    ended by Windows ("stopped responding"). Now the live game objects are released before the rebuild
    (d3d::ReleaseGame), 8 reader threads fetch contents in creation order up to 512 MB ahead of the rebuild, contents
    are read ahead before the stop only when more than 4 GB of RAM is free, and a message ("Loading savestate N from
    an earlier game session...") is drawn before the game stops. Same savestate size class then: 57k objects in 18 s.
  - Prologue savestate, other session: loaded, then the AMD driver crashed on its own thread 8 ms later. The
    savestate's outside-pointer table had ~500 pointers into rebuilt D3D objects at fixed offsets per class (+0x240
    x299, +0x1B8 x70, +0xB8 x62...): other interfaces of the same objects (DXGI surface/resource side). They are now
    mapped to the rebuilt object plus the same offset. Still open from that log: ~25 D3D pointers to objects not in
    the manifest, and 13k plain pointers from GTA5.exe's .data into the heap the D3D objects live in. Saves now log
    which DLLs create D3D objects that aren't counted as the game's, and which allocations plain pointers lead into.
  - Second Prologue test (with the interface mapping: 178 pointers mapped): same AMD driver-thread crash right after
    the load. The unmapped list was led by objects with vtables inside amdxx64.dll (38 + 25 + a few); GTA5.exe
    contains "AmdDxExtCreate11", AMD's private D3D extension API, whose objects live inside the driver and can't be
    kept or rebuilt. The game is now told the extensions don't exist (as on other GPUs). Also: the GPU is drained
    before the live objects are released and after the rebuild, and the previous launch's log is kept as
    savestate.prev.log.
- Decision (user, 2026-11-12): savestates are session-only again, because loads in a later session still crashed
  the graphics driver in the Prologue. At game start the previous session's files are deleted (slotNN.gss/.gpu and
  their .tmp, and the GPU store), except while another running game owns the folder (named mutex per folder) or a
  file's header names a process that is still running. Loads of earlier-session files are refused. The cross-session
  code stays behind [Lab] OtherSessions=1 (files saved with it carry a header flag; only those load later); without it
  no GPU contents or manifests are stored (store mode Off), no outside-pointer scan runs after a save, and AMD's
  driver extensions are offered to the game again. Savestates now keep a reference to other DLLs' GPU objects bound
  at save time (an in-session load binds them again).
- Thread-exit crash found in a lab run (Prologue replay start, no savestate involved): ntdll freed our fixed
  GTA5.exe thread-local block at thread exit. Windows had replaced the thread's TLS pointer table after a DLL with
  thread-local data was loaded, and the exit code wrote the thread's own block back into the stale table. The slot is
  now looked up in the current table at exit, the contents are copied back, and a game thread calling ExitThread
  itself is handled too (zombie while savestates exist, else the same give-back).
- Both changes were reviewed by agent workflows (four reviewers + skeptics, then attackers + skeptics on the fixes).
- User report (2026-11-12): after an in-session load, text on the phone and the minimap showed wrong letters. In-session
  loads pinned D3D objects and set their reference counts back but never restored resource contents. The game's
  Scaleform GFx 3 glyph raster cache writes glyphs into a texture during play and keeps the glyph-to-slot table in game
  memory, so after a load the restored table pointed at slots holding later glyphs. Saves now keep GPU-side copies of
  every pinned resource the game can change in place (DEFAULT usage, not a render target or immutable; staging copies
  of dynamic textures), and an in-session load copies them back before the pipeline state. An earlier object census
  puts this at ~100 resources, a few MB per savestate (streamed textures and meshes are immutable). Reviewed by an
  agent workflow; also fixed from it: a capture whose slot was overwritten while the game couldn't be stopped a second
  time leaked (pins, references, copies); such captures are now dropped at the next save or load.
  User test: one save kept 104 resources (3.5 MB); four loads (0.41-0.55 s stops), text correct after each, normal exit.
- Release 1.0.0 (2026-11-12). Low-VRAM work, reviewed by an agent audit: each save logs video memory use against
  the DXGI budget (IDXGIAdapter3) and how much only other savestates keep alive (not counting the slot being
  replaced, nor resources still used through views), and warns on screen (wrapped, 8 s) when it is nearly full.
  WDDM demotes instead of failing allocations, so an over-budget game stutters rather than crashes. Running out of
  RAM/page file while a save is collected now frees the copy at once and fails the save with a clear message, and an
  out-of-memory exception no longer ends the game. Package: release\GTA5-Savestates-1.0.0.zip (um publish check: PASS).
- 1.0.1 (2026-11-12): a tester got "Err_Sys_Thread" ~4 s after start (first loading screen; log ended without an
  exception). Project 1.27's Launch button starts the game with "start /affinity <first clamp(threads,4,16) CPUs>"
  (its "core fix"; old GTA V builds fail with ERR_SYS_THREAD on CPUs with many threads) and sets GTA5.exe to high
  priority 5 s after start; our launcher did neither. It now reads P127's settings (HKLM\SOFTWARE\Project_127:
  EnableCoreFix, EnableAutoSetHighPriority, LanguageSelected, Enable{Stutter,Audio,Crash}Fix ->
  -No*Fix, EnableOverWriteGTACommandLineArgs/OverWriteGTACommandLineArgs), with P127's defaults when absent, sets the
  affinity on PlayGTAV.exe before it runs (GTA5.exe inherits it) and the DLL raises the priority. The log now records
  the system and each game thread (all ask 16-512 KB of stack on this PC: the stack-size theory is out). Our test:
  12 CPU threads, mask FFF, priority set, save/load fine.
- 1.0.2 (2026-11-15): a report that the game crashed going from fullscreen to windowed. Every savestate pinned the
  game's views of the swap-chain back buffer (an SRV and an RTV), so IDXGISwapChain::ResizeBuffers failed with
  DXGI_ERROR_INVALID_CALL and the game read a null back buffer. Hooks on ResizeBuffers, SetFullscreenState and GetBuffer
  (vtable 13/10/9) log every switch, plus the life of the back-buffer views. Approaches tried:
  - Not pinning the views: switches work, but Alt+Tab out of fullscreen and back (SetFullscreenState with no
    ResizeBuffers) makes GTA re-create its views on the same buffer, and older savestates lose theirs.
  - Rewriting an older savestate's pointers to the new buffer and views: the load works, but at the next switch the
    game doesn't release its views, ResizeBuffers fails and the game crashes (lab\crash_switch_after_remap*.log).
  Kept: views are pinned as before. Right before ResizeBuffers, the hook makes savestates let go of the views of the
  current buffers (UnpinBufferViews) and bumps an epoch. The worker then drops and deletes the savestates made before
  (ss::DropStale), with a message. Other DLLs' bound views of the back buffer stay out of Capture::bound. Logging trap:
  when the resolution changes, GTA rebuilds its render targets on worker threads, and they reuse freed back-buffer
  addresses. Tested: Alt+Tab keeps savestates loadable, Alt+Enter deletes them, about 20 switches and 14 loads with no
  crash.
- 1.0.3: the default keys are F9 save, F10/F11 slot, F12 load (at the user's request). savestate.ini takes key names
  (main.cpp KeyFromName), and the log lists the keys in use.
## Limits (now)
- A switch between fullscreen and a window, or a resolution change (ResizeBuffers), deletes the savestates made
  before it. Alt+Tab keeps them.
- Savestates last until the game is closed (deleted at the next start). Loading them in a later session works in
  free roam and some missions but crashed the AMD driver in the Prologue; that path is lab-only (OtherSessions=1).
- Render targets and dynamic resources start empty after a cross-session load (the game redraws them); a pointer the
  game made to another DLL's object after start-up (one DirectSound object in the tests) can stay unmapped.
