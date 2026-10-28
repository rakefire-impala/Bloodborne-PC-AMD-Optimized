"""Launcher options in the launcher's folder: the %APPDATA% ones of older versions move there."""
import importlib.util
import json
from pathlib import Path
import tempfile

source = Path(__file__).resolve().parents[1] / 'launcher/bbport_launcher_win.py'
spec = importlib.util.spec_from_file_location('launcher_settings_location', source)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

assert launcher.CONFIG_FILE.parent == launcher.DATA_DIR
assert launcher.UPDATE_DIR.is_relative_to(launcher.DATA_DIR)
assert launcher.CONFIG_FILE.name in launcher.USER_FILES

with tempfile.TemporaryDirectory(prefix='bbport-settings-') as directory:
    root = Path(directory)
    launcher.CONFIG_FILE = root / 'port' / 'bbport.launcher.json'
    launcher.CONFIG_FILE.parent.mkdir()
    launcher.LEGACY_CONFIG_DIR = root / 'appdata' / 'bbport-launcher'
    legacy = launcher.LEGACY_CONFIG_DIR / 'settings.json'

    # Nothing to move.
    launcher.migrate_settings()
    assert not launcher.CONFIG_FILE.exists()

    # Moved, with the old update checker's file, and the folder goes.
    launcher.LEGACY_CONFIG_DIR.mkdir(parents=True)
    legacy.write_text(json.dumps({'game_dir': r'F:\Games\bloodborne'}), encoding='utf-8')
    (launcher.LEGACY_CONFIG_DIR / 'update-check.json').write_text('{}')
    launcher.migrate_settings()
    assert json.loads(launcher.CONFIG_FILE.read_text(encoding='utf-8')) == {'game_dir': r'F:\Games\bloodborne'}
    assert not launcher.LEGACY_CONFIG_DIR.exists()

    # Options here already: the old ones belong to another installation and stay.
    launcher.LEGACY_CONFIG_DIR.mkdir(parents=True)
    legacy.write_text('{"game_dir": "D:\\\\other"}', encoding='utf-8')
    launcher.migrate_settings()
    assert legacy.is_file()
    assert json.loads(launcher.CONFIG_FILE.read_text(encoding='utf-8'))['game_dir'] == r'F:\Games\bloodborne'

print(Path(__file__).name, 'ok')
