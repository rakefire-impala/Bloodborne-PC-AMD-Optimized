#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

# Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself.
GAME_FOLDERS = {'action', 'chr', 'event', 'facegen', 'font', 'map', 'menu', 'movie', 'msg', 'mtd',
                'obj', 'other', 'param', 'paramdef', 'parts', 'remo', 'script', 'sfx', 'shader',
                'sound'}


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names)."""
    folder = Path(folder)
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


def content_root(folder):
    """(root, prefix): where a mod's files are and the game path they go to, or None.

    Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/CUSA03173/dvdroot_ps4,
    one wrapper folder around any of these (an archive extracted into a folder of its name), and
    the game's folders without dvdroot_ps4 (<mod>/chr, <mod>/parts, ...)."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    for wrapper in ('', 'app0', 'CUSA03173'):
        base = child(folder, wrapper) if wrapper else folder
        dvdroot = child(base, 'dvdroot_ps4')
        if dvdroot.is_dir():
            return base, ''
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if folders and all(e.name.casefold() in GAME_FOLDERS for e in folders):
        return folder, 'dvdroot_ps4'
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0])
    return None


def discover(root):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and content_root(p)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config):
    available = discover(root)
    if not config or not Path(config).is_file():
        return available
    settings = json.loads(Path(config).read_text())
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    disabled = set(disabled_names)
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    # New folders are enabled automatically and appended in alphabetical order.
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder):
    """(game path, source) of every file a mod replaces or adds under dvdroot_ps4."""
    layout = content_root(folder)
    if not layout:
        raise ValueError(f'{folder}: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder')
    root, prefix = layout
    root = root.resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in [*folders, *files]:
            if (directory / name).is_symlink():
                raise ValueError(f'Mod symlinks are unsupported: {directory / name}')
        for name in sorted(files):
            source = directory / name
            relative = Path(prefix) / source.relative_to(root) if prefix else source.relative_to(root)
            # Readme/metadata stay outside the mounted game. This loader handles assets;
            # executable patches use the existing patch compiler, with address validation.
            if relative.parts[0].casefold() != 'dvdroot_ps4':
                if relative.parts[0].casefold() in ('eboot.bin', 'sce_module', 'sce_sys'):
                    raise ValueError(f'Executable/system replacement is unsupported: {source}')
                continue
            if not source.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative, source


# Windows without the symlink privilege (Developer Mode off): directories become junctions,
# files hard links (copies across volumes). Those are not symlinks, so the overlay remembers
# what it linked and where to.
LINKED = {}


def make_link(link, target):
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
        return
    except OSError:
        if os.name != 'nt':
            raise
    if target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        try:
            os.link(target, link)
        except OSError:
            shutil.copy2(target, link)
    LINKED[str(link)] = target


def symlinks_work(directory):
    probe = Path(tempfile.mkdtemp(prefix='link-test-', dir=directory))
    try:
        (probe / 'link').symlink_to(probe, target_is_directory=True)
        return True
    except OSError:
        return False
    finally:
        shutil.rmtree(probe, ignore_errors=True)


# mod-game-<pid of the launching script>-<random>; overlays from before the pid have none.
OVERLAY_NAME = re.compile(r'mod-game-(?:(\d+)-)?[a-z0-9_]{8}')
# Inside the game folder, for overlays that need the game's volume (needs_game_volume).
OVERLAY_FOLDER = 'bbport-overlays'


def process_running(pid):
    """Whether process `pid` exists (ids are reused: a wrong yes only delays a cleanup)."""
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            pass
        return True
    # os.kill would terminate the process on Windows.
    import ctypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.GetExitCodeProcess.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ctypes.get_last_error() == 5  # access denied: it exists
    try:
        code = ctypes.c_ulong()
        return not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
    finally:
        kernel32.CloseHandle(handle)


def remove_stale_overlays(directory):
    """Overlays left by earlier runs. run.py removes its own once the game exits, but not when
    it was ended itself (launcher closed, process tree ended) or a file was still in use: they
    piled up beside the game. One whose launching script still runs is in use."""
    directory = Path(directory)
    if not directory.is_dir():
        return
    for entry in directory.iterdir():
        match = OVERLAY_NAME.fullmatch(entry.name)
        if not match or is_link(entry) or not entry.is_dir():
            continue
        if match[1] and process_running(int(match[1])):
            continue
        # Links and copies only: the game and mod files they point to stay.
        shutil.rmtree(entry, ignore_errors=True)


def is_link(path):
    return path.is_symlink() or str(path) in LINKED or (hasattr(path, 'is_junction') and path.is_junction())


def link_target(path):
    return LINKED.get(str(path)) or path.resolve(strict=True)


def remove_link(path):
    if hasattr(path, 'is_junction') and path.is_junction():
        os.rmdir(path)
    else:
        path.unlink()
    LINKED.pop(str(path), None)


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if is_link(directory):
        target = link_target(directory)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        remove_link(directory)
        directory.mkdir()
        for entry in target.iterdir():
            make_link(directory / entry.name, entry)
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def needs_game_volume(game, out):
    """Hard links only reach files on the same volume: without symlinks (Windows, Developer Mode
    off) an overlay of a game on another drive has to be on the game's drive."""
    return os.name == 'nt' and not symlinks_work(out) and out.drive.casefold() != game.drive.casefold()


def remove_overlay(overlay):
    """Removes an overlay, and the game's overlay folder once it is empty (run.py, at exit)."""
    overlay = Path(overlay)
    shutil.rmtree(overlay, ignore_errors=True)
    if overlay.parent.name == OVERLAY_FOLDER:
        try:
            overlay.parent.rmdir()
        except OSError:
            pass


def build_overlay(game, out, mods):
    game = Path(game).resolve(strict=True)
    in_game = game / OVERLAY_FOLDER
    # Those of earlier runs, including the ones older versions put beside the game folder.
    for folder in (Path(out).resolve(), in_game, game.parent):
        remove_stale_overlays(folder)
    try:
        in_game.rmdir()
    except OSError:
        pass
    replacements = []
    owners = {}
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root):
            key = relative.as_posix().casefold()
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    if not replacements:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if needs_game_volume(game, out):
        # Inside the game folder rather than beside it: nothing outside the game's and the
        # launcher's folders.
        out = in_game
        out.mkdir(exist_ok=True)
    # Named after the script that runs the game (run.py, run.sh): in use while it runs.
    overlay = Path(tempfile.mkdtemp(prefix=f'mod-game-{os.getppid()}-', dir=out))
    try:
        for entry in game.iterdir():
            if entry.name.casefold() == OVERLAY_FOLDER:
                continue
            make_link(overlay / entry.name, entry)
        replaced = added = 0
        for relative, source in replacements:
            # Each component takes the game's spelling when it exists in another case.
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if is_link(destination):
                replaced += link_target(destination).is_relative_to(game)
                remove_link(destination)
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            make_link(destination, source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        return overlay
    except BaseException:
        remove_overlay(overlay)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    parser.add_argument('--lamp-travel', choices=('0', '1'), default='0',
                        help='lamp-to-lamp travel in the generated layer (lamp_travel.py)')
    parser.add_argument('--lamp-save', choices=('0', '1'), default='0',
                        help='lamps save the game, in the generated layer (lamp_travel.py)')
    parser.add_argument('--boss-retry', choices=('0', '1'), default='0',
                        help='retrying boss fights, in the generated layer (boss_retry.py)')
    args = parser.parse_args()
    layers = []
    if args.enabled == '1':
        # shadPS4's loose overlay convention, beside the original game.
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir():
            layers.append((legacy.name, legacy))
        # A directly selected loose overlay is also accepted.
        if child(args.mods_dir, 'dvdroot_ps4').is_dir():
            layers.append((args.mods_dir.name, args.mods_dir))
        else:
            for name in selected(args.mods_dir, args.config):
                layers.append((name, args.mods_dir / name))
    layer = qol_layer(args.game, args.out, layers, args.lamp_travel == '1', args.lamp_save == '1',
                      args.boss_retry == '1')
    if layer:
        layers.append(layer)
    print(build_overlay(args.game, args.out, layers))


class Generated:
    """The files in effect, with what an earlier generator wrote to the layer taking precedence
    (both patch the common event script)."""

    def __init__(self, layer: Path, files):
        self.layer, self.files = layer, files

    def __call__(self, relative):
        written = self.layer / 'dvdroot_ps4' / relative
        return written.read_bytes() if written.is_file() else self.files(relative)

    def __getattr__(self, name):
        return getattr(self.files, name)


def qol_layer(game, out, layers, travel, save, retry):
    """The Quality of Life features made of game files, generated last from the files the other
    layers leave in effect (so a mod's talk or event scripts are patched rather than replaced).
    A game version or mod a generator does not understand: play without that feature."""
    if not (travel or save or retry):
        return None
    import boss_retry
    import lamp_travel
    from patches import game_app_version
    in_effect = {}
    for _name, root in layers:
        for relative, source in mod_files(root):
            in_effect[relative.as_posix()] = source
    target = Path(out).resolve() / 'qol'
    shutil.rmtree(target, ignore_errors=True)
    files = Generated(target, lamp_travel.GameFiles(game, in_effect))
    language = lamp_travel.game_language()
    # Travel and retrying wait for the port, which reads the event flags of the 1.09 executable
    # only (src/runtime_lamp.c, runtime_retry.c): another version would leave the dialog waiting.
    version = game_app_version(game)
    if version != '01.09' and (travel or retry):
        print(f'Lamp travel, boss retry: off (game version {version}; they need 01.09)', file=sys.stderr)
        travel = retry = False
    if travel or save:
        try:
            count = lamp_travel.generate(files, target, language, travel, save)
            if travel:
                print(f'Lamp travel: {count} lamps can travel to other lamps', file=sys.stderr)
            if save:
                print(f'Lamp save: {count} lamps save the game', file=sys.stderr)
        except (OSError, ValueError, struct.error) as error:
            print(f'Lamp travel, lamp save: off ({error})', file=sys.stderr)
            shutil.rmtree(target, ignore_errors=True)
    if retry:
        try:
            count = boss_retry.generate(files, target, language)
            print(f'Boss retry: {count} boss fog gates', file=sys.stderr)
        except (OSError, ValueError, struct.error) as error:
            print(f'Boss retry: off ({error})', file=sys.stderr)
            (target / boss_retry.RETRY_FILE).unlink(missing_ok=True)
    return ('Quality of life (generated)', target) if (target / 'dvdroot_ps4').is_dir() else None


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)
