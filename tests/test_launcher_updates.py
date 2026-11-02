"""Keep updates on this fork's release channel, including the compiled Windows launcher."""
import ast
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import queue
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

source = Path(__file__).resolve().parents[1] / 'launcher/bbport_launcher_win.py'
if len(sys.argv) > 1:
    import marshal
    from PyInstaller.archive.readers import CArchiveReader
    executable = sys.argv.pop(1)
    launcher = types.ModuleType('bbport_launcher_update_test')
    launcher.__file__ = str(source)
    exec(marshal.loads(CArchiveReader(executable).extract('bbport_launcher_win')), launcher.__dict__)
else:
    spec = importlib.util.spec_from_file_location('bbport_launcher_update_test', source)
    launcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(launcher)


source_version = next(n.value.value for n in ast.parse(source.read_text(encoding='utf-8')).body
                      if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'VERSION' for t in n.targets))
parts = source_version.split('.')
future_version = '.'.join(parts[:-1] + [str(int(parts[-1]) + 1)])
FORK = 'https://github.com/rakefire-impala/Bloodborne-PC-AMD-Optimized/releases'
DIGEST = 'a' * 64


class ForkUpdates(unittest.TestCase):
    def release(self, tag=None, assets=None):
        tag = tag or 'v' + future_version
        return {'tag_name': tag, 'html_url': FORK + '/tag/' + tag,
                'assets': assets if assets is not None else [
                    {'name': 'source.zip', 'browser_download_url': 'https://example.invalid/source.zip'},
                    {'name': 'bbport-windows.zip', 'digest': 'sha256:' + DIGEST,
                     'browser_download_url': 'https://example.invalid/windows.zip'}]}

    def latest(self, release):
        with patch.object(launcher.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(release).encode())) as request:
            result = launcher.latest_release()
        self.assertEqual(request.call_args.args[0].full_url,
                         'https://api.github.com/repos/rakefire-impala/Bloodborne-PC-AMD-Optimized/releases/latest')
        return result

    def check(self, release, manual=False):
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.ui_calls = queue.Queue()
        calls = []
        app.offer_update = lambda *args: calls.append(('update', args))
        app.messagebox = types.SimpleNamespace(
            showinfo=lambda *args: calls.append(('info', args)),
            showerror=lambda *args: calls.append(('error', args)))
        with patch.object(launcher.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(release).encode())):
            app.check_update(manual)
        while not app.ui_calls.empty():
            app.ui_calls.get_nowait()()
        return calls

    def test_channel_and_exact_windows_asset(self):
        self.assertEqual(launcher.VERSION, source_version)
        self.assertEqual(launcher.RELEASES_PAGE, FORK + '/latest')
        version, url, page, sha256 = self.latest(self.release())
        self.assertEqual((version, url, sha256), (future_version, 'https://example.invalid/windows.zip', DIGEST))
        self.assertTrue(page.endswith('/v' + future_version))

    def test_source_zip_is_not_installed(self):
        self.assertIsNone(self.latest(self.release(assets=[
            {'name': 'source.zip', 'browser_download_url': 'https://example.invalid/source.zip'}]))[1])

    def test_a_release_without_a_digest_still_installs(self):
        result = self.latest(self.release(assets=[
            {'name': 'bbport-windows.zip', 'browser_download_url': 'https://example.invalid/windows.zip'}]))
        self.assertEqual((result[1], result[3]), ('https://example.invalid/windows.zip', None))

    def test_other_release_channels_are_rejected(self):
        for tag in ('windows-v1.6-tatsunakyo.23', 'windows-v99', 'v1.6-beta', 'v', '99'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                self.latest(self.release(tag))
            self.assertEqual(self.check(self.release(tag)), [])
        self.assertEqual(self.check(self.release('windows-v99'), manual=True)[0][0], 'error')

    def test_version_comparison_and_offer(self):
        self.assertEqual(self.check(self.release('v' + source_version)), [])
        self.assertEqual(self.check(self.release('v1.14.5')), [])
        self.assertEqual(self.check(self.release('v1.9')), [])
        self.assertEqual(self.check(self.release('v1.9'), manual=True)[0][0], 'info')
        self.assertEqual(self.check(self.release())[0][0], 'update')
        self.assertEqual(self.check(self.release('v' + source_version + '.1'))[0][0], 'update')
        self.assertGreater(launcher.version_tuple('1.15'), launcher.version_tuple('1.14.5'))
        self.assertGreater(launcher.version_tuple('1.15'), launcher.version_tuple('1.9'))

    def test_no_published_release_is_quiet_on_startup(self):
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.ui_calls = queue.Queue()
        error = HTTPError(launcher.RELEASES_API, 404, 'Not Found', {}, None)
        try:
            with patch.object(launcher.urllib.request, 'urlopen', side_effect=error):
                app.check_update()
        finally:
            error.close()
        self.assertTrue(app.ui_calls.empty())

    def check_with_errors(self, errors, manual=False):
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.ui_calls = queue.Queue()
        calls = []
        app.offer_update = lambda *args: calls.append(('update', args))
        app.messagebox = types.SimpleNamespace(showinfo=lambda *a: calls.append(('info', a)),
                                               showerror=lambda *a: calls.append(('error', a)))
        answers = iter(errors)

        def answer(*_args, **_kwargs):
            item = next(answers)
            if isinstance(item, Exception):
                raise item
            return item
        with patch.object(launcher.urllib.request, 'urlopen', side_effect=answer) as opened, \
                patch.object(launcher.time, 'sleep') as slept:
            app.check_update(manual)
        while not app.ui_calls.empty():
            app.ui_calls.get_nowait()()
        return calls, opened.call_count, slept.call_count

    def test_startup_check_retries_a_certificate_or_network_failure(self):
        bad = URLError('certificate verify failed: unable to get issuer certificate')
        good = io.BytesIO(json.dumps(self.release()).encode())
        calls, opened, slept = self.check_with_errors([bad, good])
        self.assertEqual((calls[0][0], opened, slept), ('update', 2, 1))
        calls, opened, slept = self.check_with_errors([bad, bad, bad])
        self.assertEqual((calls, opened, slept), ([], launcher.UPDATE_CHECK_ATTEMPTS, launcher.UPDATE_CHECK_ATTEMPTS - 1))

    def test_a_manual_check_reports_at_once(self):
        calls, opened, slept = self.check_with_errors([URLError('no network')], manual=True)
        self.assertEqual((calls[0][0], opened, slept), ('error', 1, 0))

    def test_github_answers_are_not_retried(self):
        error = HTTPError(launcher.RELEASES_API, 403, 'rate limited', {}, None)
        try:
            self.assertEqual(self.check_with_errors([error]), ([], 1, 0))
        finally:
            error.close()

    def download(self, payload, sha256):
        response = io.BytesIO(payload)
        response.headers = {'Content-Length': str(len(payload))}
        seen = []
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(launcher.urllib.request, 'urlopen', return_value=response):
            launcher.download_package('https://example.invalid/windows.zip', sha256, Path(folder) / 'update.zip', seen.append)
            return (Path(folder) / 'update.zip').read_bytes(), seen

    def test_download_checks_the_published_checksum(self):
        payload = b'package' * 1000
        good = hashlib.sha256(payload).hexdigest()
        self.assertEqual(self.download(payload, good)[0], payload)
        self.assertEqual(self.download(payload, good.upper())[1][-1], 100)
        self.assertEqual(self.download(payload, None)[0], payload)

    def test_download_stops_when_cancelled(self):
        payload = b'package' * 100000
        response = io.BytesIO(payload)
        response.headers = {'Content-Length': str(len(payload))}
        cancelled = launcher.threading.Event()
        seen = []

        def progress(percent):
            seen.append(percent)
            cancelled.set()
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(launcher.urllib.request, 'urlopen', return_value=response):
            with self.assertRaises(launcher.UpdateCancelled):
                launcher.download_package('https://example.invalid/windows.zip', None, Path(folder) / 'update.zip',
                                          progress, cancelled)
        self.assertEqual(len(seen), 1)
        with self.assertRaises(OSError):
            self.download(payload, DIGEST)


if __name__ == '__main__':
    unittest.main()
