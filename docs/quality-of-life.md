# Quality of Life

The launcher's **Quality of Life** page holds conveniences the original game lacks. They apply at
the next start and are off by default.

## Skip the intro logos and movie

The existing `Skip Intro` game patch (`skip_intro` in `bbport.ini`), moved here from Game & effects.

## Travel from any lamp to any other lamp

`qol_lamp_travel=1` in `bbport.ini` (or `BB_LAMP_TRAVEL=1`). At a lamp, "Return to the Hunter's
Dream?" opens the travel menu instead, drawn by the port:

- a tab per headstone of the Hunter's Dream (Yharnam, Frontier, Unseen, Nightmare, Hunter's
  Nightmare), switched with L1/R1 (LB/RB; keyboard 1/3, arrows or A/D); headstones without a lit
  lamp are skipped;
- the headstone's lamps in its own order: lit lamps by name, `???` for lamps not lit yet (only lamps
  you lit yourself show a name, as on the Dream's headstones), and the lamp you stand at marked;
- **Hunter's Dream** on the side, on Square (X on Xbox pads; keyboard E): the game's own travel;
- Cross (A; Enter) travels, Circle (B; Esc) returns. The mouse works too.

Picking a lamp plays the lamp travel animation and warps there. Lamp, headstone and key names are
the game's own, in the game language (Japanese, Korean and Chinese use English: the overlay font has
no CJK glyphs). The titles use Georgia when Windows has it. Single player only (lamps offer travel
only then), and the Hunter's Dream keeps its headstones unchanged.

How it is made: at launch, `scripts/mods.py` runs `scripts/lamp_travel.py` on the files in effect
(the game's, or a mod's) and mounts the result as the last mod layer (`out/qol/`, shared with lamp saving and boss retry):

- every lamp's talk script (`script/talk/<map>.talkesdbnd.dcx`): the dialog asks the port for a
  choice through event flags 6920-6971 (no game file uses 6920-6999): it clears them, marks its own
  lamp (`6928 + n`) and sets 6920; the port shows the menu and sets the lamp picked (or 6922, the
  Hunter's Dream) and then 6921. The script then runs the Dream headstones' own "is this lamp usable"
  check (`m21_00_00_00`: t210000-t210003, t210721, copied unchanged), which shows the game's message
  or sets the headstone's warp request flag (`72100000 + map * 10 + lamp`);
- `lamp_menu.txt` beside it: the headstones, their lamps (menu flag, lit flag, name) and the texts,
  passed to the port as `BB_LAMP_MENU`. `src/runtime_lamp.c` reads and sets the flags with the
  game's own event flag functions (1.09 only, checked byte for byte at start), on the thread that
  reads the pad; `gpu/shim/bbport_lamp_menu.cpp` draws the menu and holds the game's input neutral;
- `event/common.emevd.dcx` gets event 7250, one instance per lamp of the Dream's warp handler
  (event 12107000): outside the Dream and outside co-op it waits for that flag, plays animation
  101160 and warps with "respawn point warp" to the lamp's region. Like the Dream's own travel it
  leaves the flag on for the destination's arrival event (common 7300) to clear, and it first waits
  for the flag to be off, so arriving at a lamp does not warp a second time.

Nothing is distributed: the files are generated from your copy each launch. If generation fails
(another game version, a mod that rewrites lamp scripts beyond recognition), the log says
`Lamp travel: off (...)` and the game starts without it. The layer is made for game version 1.09
only, because the lamp dialogs wait for the port's menu and the port reads 1.09's event flags; a
modified 1.09 executable logs `Lamp travel: executable/version guard failed`, and its lamp dialogs
would then wait for a menu that never opens (turn the option off in that case). `scripts/fsformats.py` reads and writes
the formats involved (DCX, BND4, talk and event scripts); `tests/test_fsformats.py` and
`tests/test_lamp_travel.py` check them against the game files (`BB_GAME_DIR`).

## Lamps save the game

`qol_lamp_save=1` in `bbport.ini` (or `BB_LAMP_SAVE=1`). Using a lamp saves the game at once. The
generator (`scripts/lamp_travel.py`, with or without travel) makes each lamp's dialog set event flag
6985, and common event 7260 waits for it, clears it and requests a save (`2000[05]`, the
instruction the game's own events use).

## The world pauses while the start menu is open ("safe menu")

`qol_safe_menu=1` in `bbport.ini` (or `BB_SAFE_MENU=1`). While the start menu is open (any of its
pages: inventory, equipment, status, ...), the whole world stands still until it closes: your
character, enemies and their projectiles, elevators and other map objects, the camera, event
scripts, the world's sounds and its effects (fire, fog, ...). The menus, their sounds and items work
as usual (using an item closes the menu, as in the game). Single player only: as host, while joining
or as a guest, nothing pauses.

- The world stops the way it does during a cutscene. The field's update (1.09 `0x193ac10`) decides
  each frame whether the world runs (not during a cutscene, otherwise unless the field is ending,
  byte `+0x153`) and keeps the answer at `+0x152`, which the other world task reads too. The
  instructions computing it (`0x193b1ff`) jump to a stub that also answers "stopped" while paused.
  Verified: the player cannot walk or turn the camera; the Hemwick elevator stopped mid-ride.
- Enemies also get the game's own debug freeze ("Switch All No Update", bit `0x400` of each
  character's debug flags) and projectiles three skipped calls; both came first and stay.
- Sounds (FMOD Ex): the event channel groups playing under each category are paused at the start
  of `EventSystemI::update` and resumed when the menu closes; the menu's own sounds play.
- Effects: the graphics scene's update has an unused switch (scene `+0x5ce`) that gives the
  particle effects no time; they are drawn as they are.
- Nothing is made invulnerable: a fall or poison simply waits for the menu to close.

`src/runtime_pause.c` checks the 1.09 executable byte for byte before hooking; otherwise it logs
`Safe menu: executable/version guard failed` and the menu does not pause.

## The world pauses while the game window is not focused

`qol_focus_pause=1` in `bbport.ini` (or `BB_FOCUS_PAUSE=1`). The same stop as the safe menu's,
with or without it, while the game window does not have input focus (Alt+Tab, a click on another
window); it ends when the window gets focus back. The game keeps drawing (the launcher's "When the
window loses focus" choice still sets its frame rate, or pauses the whole game instead). Single
player only, 1.09 only, like the safe menu.

## Retry a boss fight after dying in it

`qol_boss_retry=1` in `bbport.ini` (or `BB_BOSS_RETRY=1`). After a death to a story boss (a boss bar
naming a boss, not an NPC hunter) behind a fog gate, once you are on your feet again after the
loading screen the overlay asks "Retry the fight?" (Cross/A/Enter: retry; Circle/B/Esc: not now).
Retrying is one loading screen, after which you stand in front of the boss's fog, facing it, with
its prompt to walk through. If you die again you come back at the lamp of that map that is always
there (not the one the boss leaves in its arena). Single player, 1.09 only, outside the Chalice
Dungeons (their maps are generated).

`scripts/boss_retry.py` finds the gates in the map event scripts (the host event turning the player
toward a point with the fog walk, animation 101130; its defeat flag, the fog object and the region
inside the fog) and adds, per gate:
- a map event that writes the gate's number to flags 6989-6996 and sets 6988 when you walk in
  while the boss lives;
- a region a step in front of the fog (the "spot"), in every layout of the map
  (`map/MapStudio/mAA_BB_00_*.msb.dcx`): the game has no entity there (the point the fog walk
  turns to is in the arena, at Darkbeast Paarl under its floor, and a short warp to the fog object
  does nothing);
- common events 7400-7599: on flag 6986 with the gate's number, a respawn-point warp (lamps only)
  to the lamp of the map nearest the fog; on arrival, a short warp (EMEVD 2004[41]; 2004[03] would
  be a second loading screen) to the spot, and the respawn point set to the nearest lamp that is
  always there, as the nearest lamp is often the one in the arena.

`src/runtime_retry.c` reads the boss bars like the Discord presence, notices the death (player HP 0
during the fight) and asks once the loading screen is gone; the list is `out/qol/boss_retry.txt`
(`BB_BOSS_RETRY_LIST`).

## Planned, not implemented yet

- **Jump on Cross/A**: an automatic dash (hold Circle) then L3 (the game's only jump, `W_Jump`,
  behaviour event 1142), with item pickups and other prompts taking priority
  (`SprjActionButtonMan`, global `0x5540048`) and nothing in menus.
