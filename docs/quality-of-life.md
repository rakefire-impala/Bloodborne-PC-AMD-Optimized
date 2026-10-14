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
(the game's, or a mod's) and mounts the result as the last mod layer (`out/qol-lamp-travel/`):

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

## Planned, not implemented yet

- **Pause while the start menu is open ("safe menu")**: enemies frozen, no damage, inventory usable.
  Groundwork in the 2026-10-13 handoff: the menu manager object at image `0x5562878`, the start tab
  dialog update at `0x16ac710`, `WorldChrManDbg` flags at `0x553e888+` (player no-dead `+0`,
  stealth `+6`, silent `+7`; the "all no move/attack/damage" bytes are still to be identified).
- **Jump on Cross/A**: an automatic dash (hold Circle) then L3 (the game's only jump, `W_Jump`,
  behaviour event 1142), with item pickups and other prompts taking priority
  (`SprjActionButtonMan`, global `0x5540048`) and nothing in menus.
