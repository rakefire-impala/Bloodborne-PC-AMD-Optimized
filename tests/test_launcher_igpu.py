"""Graphics card choice and integrated GPU mode of the Windows launcher, without a display."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import tkinter as tk
import types

source = Path(__file__).resolve().parents[1] / 'launcher/bbport_launcher_win.py'
spec = importlib.util.spec_from_file_location('bbport_launcher_igpu_test', source)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class Widget:
    """The widget protocol the page builders use; variable traces run in real Tcl."""
    nodes = []

    def __init__(self, parent=None, **kwargs):
        self.parent, self.kwargs, self.bindings, self.index, self.rows = parent, kwargs, {}, 0, 0
        self.nodes.append(self)

    def grid(self, **kwargs):
        if self.parent:
            self.parent.rows = max(self.parent.rows, kwargs.get('row', 0) + 1)

    def grid_remove(self): pass
    def pack(self, **kwargs): return self
    def columnconfigure(self, *args, **kwargs): pass
    def grid_size(self): return (2, self.rows)
    def bind(self, event, callback): self.bindings[event] = callback
    def configure(self, **kwargs): self.kwargs.update(kwargs)
    def current(self, index=None):
        if index is not None: self.index = index
        return self.index
    def choose(self, index):
        self.index = index
        self.bindings['<<ComboboxSelected>>'](None)


interpreter = tk.Tcl()
tk_api = types.SimpleNamespace(
    Frame=Widget, Canvas=Widget, Label=Widget, TclError=tk.TclError,
    BooleanVar=lambda **kwargs: tk.BooleanVar(master=interpreter, **kwargs),
    StringVar=lambda **kwargs: tk.StringVar(master=interpreter, **kwargs),
    DoubleVar=lambda **kwargs: tk.DoubleVar(master=interpreter, **kwargs))
ttk_api = types.SimpleNamespace(Frame=Widget, Label=Widget, Combobox=Widget, Checkbutton=Widget,
                                Button=Widget, Scale=Widget, Spinbox=Widget, Progressbar=Widget)
GPUS = [{'kind': 'discrete', 'mib': 3824, 'vulkan13': True, 'selected': True, 'name': 'Radeon RX 5500M'},
        {'kind': 'integrated', 'mib': 256, 'vulkan13': True, 'selected': False,
         'name': 'AMD Radeon(TM) Graphics'}]


def build(settings):
    obj = launcher.Launcher.__new__(launcher.Launcher)
    obj.tk, obj.ttk, obj.dpi = tk_api, ttk_api, 1
    obj.app = {**launcher.APP_DEFAULTS, **settings}
    obj.ini, obj.ini_lines = launcher.load_ini()
    obj.menu = launcher.menu_values(obj.app)
    obj.menu_baseline = dict(obj.menu)
    obj.vars, obj.pages, obj.content = {}, {}, Widget()
    obj.mod_order, obj.patch_vars = [], {}
    obj.gpus, obj.gpu_boxes, obj.igpu_buttons = [], [], []
    obj.scrolled_page = lambda *args: Widget()
    obj.version_warning = lambda *args: None
    obj.refresh_status = lambda *args: None
    obj.build_play()
    obj.build_graphics()
    obj.build_display()
    obj.igpu_button(Widget())  # the bottom bar's, made by build()
    return obj


def gpu_boxes():
    return [w for w in Widget.nodes if hasattr(w, 'choices')]


def profile(obj):
    return {key: obj.var(key, store).get() for store, key in launcher.GPU_PROFILE_KEYS}


with tempfile.TemporaryDirectory(prefix='bbport-igpu-') as directory:
    root = Path(directory)
    launcher.DATA_DIR = root
    launcher.CONFIG_DIR = root / 'launcher'
    launcher.CONFIG_FILE = launcher.CONFIG_DIR / 'settings.json'
    previous = os.environ.get('BB_CONFIG')
    os.environ['BB_CONFIG'] = str(root / 'bbport.ini')
    try:
        launcher.LANG = 'en'
        env = launcher.game_environment(dict(launcher.APP_DEFAULTS))
        assert 'BB_GPU' not in env or env['BB_GPU'] == os.environ.get('BB_GPU')
        assert launcher.game_environment({**launcher.APP_DEFAULTS, 'gpu': 'AMD Radeon(TM) Graphics'})[
            'BB_GPU'] == 'AMD Radeon(TM) Graphics'

        obj = build({})
        boxes = gpu_boxes()
        assert len(boxes) == 2  # Play and Graphics
        assert all(box.choices == [''] for box in boxes)  # Auto until the check lists the GPUs
        obj.gpus = GPUS
        for show in obj.gpu_boxes:
            show()
        assert all(box.choices == ['', 'Radeon RX 5500M', 'AMD Radeon(TM) Graphics'] for box in boxes)
        assert boxes[0].kwargs['values'][2] == 'AMD Radeon(TM) Graphics (integrated)'
        boxes[1].choose(1)  # the manual choice: the discrete GPU by name
        assert obj.vars['gpu'].get() == 'Radeon RX 5500M' and boxes[0].current() == 1
        boxes[0].choose(0)
        assert obj.vars['gpu'].get() == '' and boxes[1].current() == 0

        # Settings for the discrete GPU, then the integrated GPU mode.
        mine = {'upscaler': 'fsr4', 'preset': '2', 'object_motion': True, 'effect_dof': True,
                'effect_ssao': True, 'model_lod': '-2', 'fps_mode': 'uncap', 'readbacks': '',
                'output_res': '2560x1440', 'frame_generation': 'fsr'}
        for store, key in launcher.GPU_PROFILE_KEYS:
            if key in mine:
                obj.var(key, store).set(mine[key])
        before = profile(obj)
        buttons = obj.igpu_buttons
        assert len(buttons) == 2 and all('off' in b.kwargs['text'] for b in buttons)
        buttons[0].kwargs['command']()
        assert obj.app['igpu_mode'] and all(b.kwargs['style'] == 'ToggleOn.TButton' for b in buttons)
        expected = {**launcher.IGPU_PROFILE, 'gpu': 'AMD Radeon(TM) Graphics'}
        now = profile(obj)
        assert all(now[key] == value for key, value in expected.items()), (now, expected)
        assert all(box.current() == 2 for box in boxes)
        ini = launcher.load_ini()[0]
        assert ini['upscaler'] == 'fsr3' and ini['object_motion'] == '0' and ini['effect_dof'] == '0'
        saved = json.loads(launcher.CONFIG_FILE.read_text(encoding='utf-8'))
        assert saved['igpu_mode'] is True and saved['gpu'] == 'AMD Radeon(TM) Graphics'
        assert saved['gpu_profiles']['discrete']['upscaler'] == 'fsr4'
        environment = launcher.game_environment(saved)
        assert environment['BB_GPU'] == 'AMD Radeon(TM) Graphics' and environment['BB_READBACKS'] == '0'

        # A change made in the mode is its own; turning it off brings the earlier settings back.
        obj.var('preset', 'ini').set('3')
        buttons[1].kwargs['command']()
        assert not obj.app['igpu_mode'] and profile(obj) == before
        assert launcher.load_ini()[0]['upscaler'] == 'fsr4' and launcher.load_menu()['frame_generation'] == 'fsr'
        buttons[0].kwargs['command']()
        assert obj.var('preset', 'ini').get() == '3' and obj.var('upscaler', 'ini').get() == 'fsr3'
        obj.reset_igpu_profile()
        assert obj.var('preset', 'ini').get() == launcher.IGPU_PROFILE['preset']
        obj.set_igpu_mode(True)  # already on: nothing changes
        assert obj.var('preset', 'ini').get() == launcher.IGPU_PROFILE['preset']
        obj.set_igpu_mode(False)
        assert profile(obj) == before

        # Without a saved profile and before the check lists the GPUs, the mode asks for the
        # first integrated GPU.
        obj.gpus = []
        obj.reset_igpu_profile()
        assert 'integrated' not in obj.app['gpu_profiles']
        obj.set_igpu_mode(True)
        assert obj.vars['gpu'].get() == launcher.GPU_INTEGRATED
        assert boxes[0].kwargs['values'][-1] == 'Integrated GPU'
        obj.set_igpu_mode(False)
        assert obj.vars['gpu'].get() == ''
    finally:
        if previous is None: os.environ.pop('BB_CONFIG', None)
        else: os.environ['BB_CONFIG'] = previous

print('PASS: graphics card choice (Auto, devices by name, shared by Play and Graphics), BB_GPU, '
      'integrated GPU mode applying its profile, restoring and keeping both profiles, reset.')
