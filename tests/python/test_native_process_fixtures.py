"""Portable inputs and executable binding for the host-only native process orchestrators."""
import importlib.util
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

STANDALONE = Path(__file__).resolve().parents[1] / 'standalone'
with patch.object(sys, 'path', [str(STANDALONE)] + sys.path):
    spec = importlib.util.spec_from_file_location(
        'native_process_support', STANDALONE / 'run-spec182-native-grant-process.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)


class NativeProcessFixtureTest(unittest.TestCase):
    def test_actor_environment_owns_mutable_session_state(self):
        with tempfile.TemporaryDirectory() as directory:
            base = {'NDNSF_CONFIG': '/etc/ndn/ndnsf.conf', 'HOME': '/unrelated'}
            selected = support.actor_environment(base, Path(directory))
            self.assertEqual(selected['NDNSF_CONFIG'],
                             str(Path(directory) / 'ndnsf-session.conf'))
            self.assertEqual(selected['HOME'], str(Path(directory) / 'home'))
            self.assertEqual(selected['NDNSF_REQUEST_LARGE_DATA_DIR'],
                             str(Path(directory) / 'large-data'))
            self.assertEqual(base['NDNSF_CONFIG'], '/etc/ndn/ndnsf.conf')
            with self.assertRaises(ValueError):
                support.actor_environment(base, Path('relative-store'))

    def test_svs_schema_uses_only_the_declared_fixture_actor_roles(self):
        with patch.dict(os.environ, {'NDNSF_TEST_FIXTURE_ROOT':
                        str(STANDALONE.parent / 'fixtures')}):
            schema = support.actor_trust_schema(
                ['/example/hello/authority', '/example/hello/provider-a'])
            self.assertEqual(schema.count('<authority|8=authority|provider-a|8=provider-a>'), 2)
            self.assertNotIn('<user|8=user|provider|8=provider|controller|8=controller>', schema)
            with self.assertRaises(ValueError):
                support.actor_trust_schema(['/example/hello/actor|.*'])
            with self.assertRaises(ValueError):
                support.actor_trust_schema([])

    def test_actor_certificates_are_valid_before_process_start(self):
        calls = []

        def public_certificate_runner(command, **kwargs):
            calls.append(command)
            return SimpleNamespace(stdout=b'public certificate process fixture')

        with tempfile.TemporaryDirectory() as directory:
            earliest = datetime.now(timezone.utc) - timedelta(minutes=1, seconds=1)
            support.provision_actor_identities(Path(directory), {},
                ['/example/hello/provider'], runner=public_certificate_runner)
            latest = datetime.now(timezone.utc) - timedelta(minutes=1)
        command = next(row for row in calls if row[1] == 'cert-gen')
        self.assertIn('--not-before', command)
        timestamp = command[command.index('--not-before') + 1]
        actual = datetime.strptime(timestamp, '%Y%m%d%H%M%S').replace(tzinfo=timezone.utc)
        self.assertLessEqual(earliest, actual)
        self.assertLessEqual(actual, latest)

    def test_offer_key_prefix_comes_from_the_actual_certificate(self):
        from ndn.encoding import Name
        certificate = '/fixture/provider/KEY/%00%FF/test-issuer/v=1'
        key = support.certificate_key_name(certificate)
        self.assertEqual([bytes(value) for value in Name.from_str(key)],
                         [bytes(value) for value in Name.from_str(certificate)[:-2]])
        with self.assertRaisesRegex(ValueError, 'v2 key suffix'):
            support.certificate_key_name('/fixture/provider/OTHER/key/issuer/v=1')

    def test_grant_namespace_maps_resolved_library_files_without_old_checkouts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / 'sdk/libexample.so.1'
            library.parent.mkdir()
            library.write_bytes(b'fixture dependency')
            report = SimpleNamespace(returncode=0,
                stdout='libexample.so.1 => ' + str(library) + ' (0x123)\n')
            with patch.object(support.subprocess, 'run', return_value=report):
                command = support.probe_command(root / 'probe', root / 'request/probe.json',
                    root / 'request', root / 'request-identity', root / 'nfd.sock',
                    root / 'build', {})
            mounts = [tuple(map(str, command[i + 1:i + 3]))
                      for i, value in enumerate(command) if value == '--ro-bind']
            self.assertIn((str(library), str(library)), mounts)
            self.assertNotIn((str(library.parent), str(library.parent)), mounts)
            self.assertFalse(any('/home/tianxing/' in str(value) for value in command))
            self.assertFalse(any('authority' in str(value) for value in command))

    def test_unresolved_probe_dependency_is_a_preflight_error(self):
        report = SimpleNamespace(returncode=0, stdout='libexample.so.1 => not found\n')
        with patch.object(support.subprocess, 'run', return_value=report):
            with self.assertRaisesRegex(RuntimeError, 'unresolved ELF dependency'):
                support.probe_libraries(Path('/candidate/probe'), {})

    def test_explicit_fixture_root_and_missing_override(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'controller-policies.conf').write_text('owned fixture')
            with patch.dict(os.environ, {'NDNSF_TEST_FIXTURE_ROOT': directory}):
                self.assertEqual(support.fixture_path('controller-policies.conf'),
                                 root / 'controller-policies.conf')
            with patch.dict(os.environ, {'NDNSF_TEST_FIXTURE_ROOT': str(root / 'missing')}):
                with self.assertRaises(FileNotFoundError):
                    support.fixture_path('controller-policies.conf')
            with patch.dict(os.environ, {'NDNSF_TEST_FIXTURE_ROOT': ''}):
                with self.assertRaises(ValueError):
                    support.fixture_path('controller-policies.conf')

    def test_fixture_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'fixtures').mkdir()
            (root / 'outside').write_text('not in fixture root')
            (root / 'fixtures/escaped').symlink_to(root / 'outside')
            with patch.dict(os.environ, {'NDNSF_TEST_FIXTURE_ROOT': str(root / 'fixtures')}):
                with self.assertRaises(ValueError):
                    support.fixture_path('../outside')
                with self.assertRaises(ValueError):
                    support.fixture_path('escaped')

    def test_installed_executable_never_falls_back_to_source_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            actor = root / 'build/examples/actor'
            actor.parent.mkdir(parents=True)
            actor.write_text('test-owned executable')
            actor.chmod(0o700)
            with patch.dict(os.environ, {}, clear=True):
                self.assertEqual(support.native_program(root / 'build', 'actor'), actor)
            with patch.dict(os.environ, {'NDNSF_TEST_RUNTIME_PREFIX': str(root / 'installed')}):
                with self.assertRaises(RuntimeError):
                    support.native_program(root / 'build', 'actor')

    def test_child_runs_in_its_owned_work_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'child.log'
            child = support.run([sys.executable, '-c',
                                 'from pathlib import Path; print(Path.cwd())'],
                                os.environ.copy(), log)
            self.assertEqual(child.returncode, 0)
            self.assertEqual(log.read_text().strip(), directory)


if __name__ == '__main__':
    unittest.main()
