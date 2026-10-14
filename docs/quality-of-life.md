# Quality of Life

The launcher's **Quality of Life** page holds conveniences the original game lacks. They apply at
the next start and are off by default.

## Skip the intro logos and movie

The existing `Skip Intro` game patch (`skip_intro` in `bbport.ini`), moved here from Game & effects.

## Travel from any lamp to any other lamp

`qol_lamp_travel=1` in `bbport.ini` (or `BB_LAMP_TRAVEL=1`). At a lamp, "Return to the Hunter's
Dream?" becomes a list: **Hunter's Dream** (the game's own travel) and the five headstones (Yharnam,
Frontier, Unseen, Nightmare, Hunter's Nightmare). A headstone opens its list of lit lamps, minus
the lamp you stand at; picking one plays the lamp travel animation and warps there. The texts are
the game's own, so the menus follow the game language. Single player only (lamps offer travel only
then), and the Hunter's Dream keeps its headstones unchanged.

How it is made: at launch, `scripts/mods.py` runs `scripts/lamp_travel.py` on the files in effect
(the game's, or a mod's) and mounts the result as the last mod layer (`out/qol-lamp-travel/`):

- every lamp's talk script (`script/talk/<map>.talkesdbnd.dcx`) gets the list menus, built from the
  Dream headstones' own lists (`m21_00_00_00`: t210000-t210003, t210721) and their "is this lamp
  usable" check, copied unchanged; picking a lamp sets the headstone's warp request flag
  (`72100000 + map * 10 + lamp`);
- `event/common.emevd.dcx` gets event 7250, one instance per lamp of the Dream's warp handler
  (event 12107000): outside the Dream and outside co-op it waits for that flag, clears it, plays
  animation 101160 and warps with "respawn point warp" to the lamp's region.

Nothing is distributed: the files are generated from your copy each launch. If generation fails
(another game version, a mod that rewrites lamp scripts beyond recognition), the log says
`Lamp travel: off (...)` and the game starts without it. `scripts/fsformats.py` reads and writes
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
