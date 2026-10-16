SkyrimMario - Milestone 5.6 (final pass: cave loop fixed) (Skyrim LE 1.9.32 + SKSE 1.7.3)
==========================================================
Mario (physics from your 32-bit sm64.dll) is drawn inside Skyrim LE and now collides
with real Skyrim geometry.

INSTALL
  Copy bin\Data\SKSE\Plugins\SkyrimMario.dll over the old one in
    C:\Games\The Elder Scrolls V - Skyrim\Data\SKSE\Plugins\
  sm64.dll and baserom.us.z64 stay in  ...\Skyrim\mario64\  as before.
  Launch with skse_loader.exe, load a save (or New Game), stand somewhere flat.

CONTROLS (game window focused)
  F9            spawn / remove Mario in front of the player (also ends possess mode)
  F8            POSSESS MODE on/off (needs Mario spawned): hides the player's body and moves the
                player actor to Mario every frame, so Skyrim's own camera follows him
  F10           flip screen Y (only if Mario appears upside-down)
  Normal mode:  Arrow keys move, Right Ctrl = A (jump), Right Shift = Z, Right Alt = B
  Possess mode: W A S D move (relative to the camera), Space = A (jump), Left Ctrl = Z
                (crouch / ground pound), F = B (punch / dive). Arrow keys still work too.
                Mouse looks around as usual.

5.6 CHANGES (the real cave bug):
  * Your last log showed 'player moved ~1800 units in one frame' about 25 times in a row, each respawn ~1795 units further along
    -Y. My 5.5 respawn created a fresh Mario but left the OLD Mario's position in the state variable, so the follower sent the
    player to 'new origin + old Mario coordinates'; that looked like a game teleport, which triggered another respawn, and so on
    until the player was thousands of units out in the void. Fixed: all per-Mario state is reset on spawn, following only starts
    after the new Mario's first physics tick, game-teleport detection has a 2 s cool-down, and spawning tries lower heights (cave
    ceilings) before falling back to the flat floor.

5.5 CHANGES (cave bug):
  * Your log: entering a cave showed Fader/Loading menus; afterwards the plugin kept moving the player to Mario's old OUTDOOR
    coordinates inside the new INTERIOR cell, which dropped you into an empty void ('flat plane'). Fix: while possessing, the
    player is kept within a few units of Mario every frame, so a jump of 600+ units in one frame can only be the game moving
    the player (load door, fast travel, respawn). The plugin now forgets the old follow target and re-spawns Mario at the
    player's new position, with a fresh SM64 origin and fresh collision (log line: 'player moved N units in one frame').
    Works in or out of possess mode.

5.4 CHANGES (freezes):
  * Your log: re-centring worked (10 re-origins, no STUCK). The remaining freezes match the collision harvest, which ran
    7-9 times per 5 s and cost 15-24 ms each on the game's main thread (a whole frame at 30-60 fps).
    Cause: for ~10,000 scene objects per harvest it did RTTI string comparisons, a memory-validity probe, and built
    several name strings per shape BEFORE checking whether the shape was anywhere near Mario.
    Now: object types are cached per vtable, the cheap distance cull runs first (names are only checked for the few
    shapes near Mario), per-object memory probes are gone, and the 'Sky' and 'Weather' branches are skipped.
    Expect the harvest line in the perf block to drop from ~15-24 ms to a few ms.
  * Baseline note: in the menu windows of your log (no Mario) the game itself ran ~24-43 fps on that capture, so ~30 fps
    in play is largely the machine/capture; the dips to 16-25 fps were the harvest windows.

5.3 CHANGES:
  * ROOT CAUSE OF THE 'STUCK' REPORT (from your log): Mario had run ~18,000 Skyrim units from his spawn point, i.e. to SM64
    coordinates around (-23095, -2321, -11540). SM64's physics only work within about +-8192 units of their origin, so
    out there it found no floor at all (the 'near:' dump showed a normal road right under him). Fix: when Mario is
    ~3000 SM64 units from the origin the SM64 world is re-centred on him: collision is harvested around his current
    Skyrim position with that position as the new origin, then in one frame the origin is swapped, the surfaces are
    loaded and Mario is moved to the matching spot (mesh/interpolation shifted too, so there is no visual pop).
    The log prints 're-origin #N' lines. Harvests made with an old origin are discarded.
  * Merged the Codex/Grok collision edits that were sound: flora/foliage meshes are not solid (name filter), and only
    walkable faces (normal.z >= 0.65) are non-slippery while steeper faces use the default surface type; plus the
    gentler harvest cadence (500 units / 8 s).
  * Your perf block looked healthy: every stage < ~2 ms on average (harvest 15 ms every few seconds on the game's
    main thread).

5.2 CHANGES:
  * Video 2: ~18 fps average but mostly >=30 fps with 37 stalls over 0.1 s (worst 0.9 s) - the periodic stalls are
    most likely the collision reload (sm64 surface load of ~30k triangles). Harvest radius 1400 -> 1000 and only a
    vertical band (-450..+550 units) around Mario is loaded; refresh every ~350 units / 4 s.
  * 'Stuck on collision': every Skyrim surface is now SURFACE_NOT_SLIPPERY (climbable), so Mario no longer slides
    off or fails to climb steep rocks/slopes (SM64's default threshold is ~38 degrees).
  * Stuck detector + debug: if you push the stick for 0.5 s without moving, the log gets 'STUCK?' and a list of
    'near:' lines naming the meshes closest to Mario (shape name, parent, class, distance, normal). F7 does the
    same on demand. Send me those lines and I will filter out the offending meshes (foliage cards, clutter...).

5.1 CHANGES (performance):
  * The video showed ~10 fps with stalls up to 0.5 s. Fixes: the overlay now draws only from the last back-buffer
    scene of each frame (it used to capture/restore the whole D3D state at EVERY back-buffer scene); collision is
    re-harvested only when Mario moved ~450 units or every 4 s (was every 0.5 s) and sm64 surfaces are only reloaded
    when the triangle set actually changed; the player-body hide pass runs every 10th frame; the player is only moved
    when more than 4 units off Mario.
  * NEW PROFILER: every 5 s (first 2 minutes) SkyrimMario.log gets a 'perf' block with avg/max milliseconds for
    overlay frame, Mario tick, geometry build, D3D draw, collision update, surface load, possess task (and its
    MoveRefr / hide parts), harvest task, audio tick, and the number of back-buffer scenes per frame.
    Please send that log after ~30 s of possessed play - it shows exactly what is still slow.

5.0 CHANGES:
  * GAMEPAD (possess mode, XInput pad #1): left stick = move, A = jump, B or X = punch/dive, left trigger = Z
    (crouch / ground pound). Right stick still turns the camera. Skyrim does not see those controls while you
    are possessing (XInputGetState hook), so the hidden body will not run around. Menus get everything back.
  * AUDIO: Mario's real N64 sound engine (jump 'Yah!', steps, landing...) plays through the Windows audio device,
    32 kHz stereo, ~100 ms buffer. It is silenced while a menu is open or Mario is removed. Volume is 0.8 in
    sm64.dll (overlay.cpp: set_sound_volume). No music is started - Skyrim keeps its own.
  * CAMERA LAG: in possess mode Mario is now drawn exactly where the player actor is (the camera follows the
    actor), instead of where Mario's physics are a frame ahead. Physics and audio threads share a lock because
    sm64.dll keeps global state.
  * Log lines to check: 'audio: waveOut started', 'xinput: patched import table' (or 'hot-patched').

4.2 CHANGES (re-based on the 4.1 sources; Codex/Grok edits reverted):
  * Menu detection uses the REAL MenuManager (singleton 0x012E3548, IsMenuOpen 0x00A5CE90). Mario is hidden,
    paused and un-controllable while a blocking menu is open (journal, inventory, map, console, loading, dialogue...).
    The log prints 'menu state:' lines whenever the state changes.
  * Possess mode now hides Mario's keys (W A S D, Space, Left Ctrl, F) from the game through a hook on the real
    dinput8.dll keyboard device, so the hidden player body stops running/jumping - that run animation is what was
    shaking the camera. Key releases still pass through, so no stuck keys. Menus get their keys back.
  * Player follow is the 4.1 behaviour again (move the player actor to Mario every frame).

4.1 CHANGES: Mario (and the camera-follow target) are now interpolated between 30 Hz physics ticks to
  remove the screen shake; the player's body is hidden by flagging every geometry in the skeleton
  (the whole-node flag was being reset by the game); the log gets 'possess: drift' lines showing how
  far the player actor lags behind Mario.

POSSESS MODE - READ THIS
  * Use a throw-away save. The player actor is teleported every frame, which can look/feel odd
    (stutter, brief falling state, possible fall damage or the game's own movement fighting back).
  * The game still receives your WASD/Space presses; the teleport simply overrides them.
  * F8 again (or F9) shows the body again and stops following.
  * The log gets a few 'possess:' lines. If the game crashes or Mario stops moving, send
    skse.log + SkyrimMario.log.

WHAT'S NEW IN MILESTONE 3: COLLISION
  * Every 0.5 s a task running on Skyrim's main thread walks the world scene graph and copies
    the triangles of static meshes within ~1400 units of Mario into sm64.dll as his floor/walls/ceilings.
    (Hidden objects and skinned actors are skipped; the player's own body is excluded.)
  * F9 now waits for the first harvest, then drops Mario ~100 units above the player's feet.
    If nothing usable is found (or the harvest times out) it falls back to the old flat floor
    and says so in the log.
  * Still an overlay: no occlusion by walls, no world lighting, draws over the HUD.
  * The player still moves with WASD; Mario is separate. Replacing the player is next.

COLLISION FIX (3.1): the first build's bounds check discarded the whole scene (log said
  nodes=0 shapes=0), so Mario only ever used the flat fallback floor. Fixed; the log now also has a
  one-time 'scene dump' of the scene graph's top levels for diagnosis.

DIAGNOSTICS (3.2): collision now loads (30k+ triangles) but Mario hovered ~130 units up. The log now
  has 'column:' lines listing every mesh triangle directly under Mario (name, height relative to the
  player's feet, normal direction) plus a once-a-second 'mario:' line with the floor height sm64 sees.
  Send me SkyrimMario.log after pressing F9 and letting him stand for ~5 seconds.

FIX (3.3): the 'floor' Mario stood on was the sky's fog layer (15_CDFog) 136 units above the player.
  Sky meshes, water planes, LOD stand-in objects and triangles with an edge >2500 units are now
  excluded from collision. The log shows what was rejected and a per-branch triangle breakdown.

IF SOMETHING LOOKS WRONG - send me SkyrimMario.log (Data\SKSE\Plugins\), it records
  hook installation, camera data and the first few frames (triangle counts, projection
  failures, positions). Useful symptoms:
  * Only visible over menus: fixed in this build (draws on every end-of-scene so it stays on top).
  * Mario falls through the ground / stands on air: the log lines starting with "harvest:" show
    how many triangles were found and how many shapes had no CPU-side data (noData=).
  * Nothing appears:      log line "frame: ... drawn=0 projFail=N" tells me if projection failed.
  * Upside-down Mario:    press F10.
  * Crash on startup:     send skse.log + SkyrimMario.log.

Rebuild (optional): MSYS2 "MinGW 32-bit" shell, run: make    (sources in src\)
