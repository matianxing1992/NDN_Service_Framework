"""Selection/reporting contracts; native protocol behavior remains in C++ tests."""
import importlib.util
import contextlib
import hashlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('ndnsf_checks',
    Path(__file__).resolve().parents[1] / 'run-checks.py')
checks = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checks)


class ConfiguredRosterContractTest(unittest.TestCase):
    def test_selected_model_requires_manifest_even_if_old_cache_directory_exists(self):
        roster = {'profiles': {'model': {'targets': [{'assets': ['qwen']},
                                                      {'assets': ['qwen']}]}},
                  'assets': {'qwen': {'recipe': 'qwen3-0.6b-int4'}}}
        with tempfile.TemporaryDirectory() as directory, patch.dict(
                os.environ, {'NDNSF_DI_MODEL_CACHE': directory}, clear=True):
            Path(directory, 'canonical-source.onnx').write_bytes(b'not a model')
            missing = checks.check_assets(roster, ['model'], None)
            self.assertEqual(len(missing), 1)
            self.assertIn('SEALED manifest required', missing[0][1])

    def test_manifest_argument_is_parsed_and_cannot_mean_search_its_parent(self):
        asset = 'qwen3-0.6b-int4-frozen-export'
        roster = {'profiles': {'model': {'targets': [{'assets': [asset]}]}},
                  'assets': {asset: {'recipe': 'qwen3-0.6b-int4'}}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / asset).mkdir()
            (root / asset / 'canonical-source.onnx').write_bytes(b'not a model')
            manifest = root / 'asset-manifest.json'
            manifest.write_text('{"state":"SEALED"}')
            bindings = {}
            missing = checks.check_assets(roster, ['model'], str(manifest), bindings)
            self.assertEqual(len(missing), 1)
            self.assertIn('RECIPE_MISMATCH', missing[0][1])
            self.assertEqual(bindings, {})

    def test_case_discovery_cannot_open_user_identity_state(self):
        original = {'HOME': '/private/home', 'NDN_CLIENT_PIB': 'private-pib',
                    'NDN_CLIENT_TPM': 'private-tpm',
                    'NDNSF_CONTROLLER_CERT_FILE': '/private/controller.cert'}
        observed = []
        def discover(argv, environ, timeout=None, *, cwd=None):
            env = environ
            observed.append(env.copy())
            self.assertEqual(Path(cwd).parent, Path(env['HOME']).parent)
            self.assertTrue(Path(env['HOME']).is_dir())
            self.assertTrue(Path(env['NDNSF_REQUEST_LARGE_DATA_DIR']).is_dir())
            self.assertTrue(Path(env['TMPDIR']).is_dir())
            self.assertEqual(Path(env['TMPDIR']).parent, Path(env['HOME']).parent)
            self.assertFalse(Path(env['HOME'], '.ndn').exists())
            self.assertNotIn('NDN_CLIENT_PIB', env)
            self.assertNotIn('NDN_CLIENT_TPM', env)
            self.assertNotIn('NDNSF_CONTROLLER_CERT_FILE', env)
            return Namespace(returncode=0, stdout='', stderr='Unit*\n    Case*\n', boundary=None)
        with patch.object(checks, 'native_process', side_effect=discover):
            self.assertEqual(checks.enumerate_cases('/fixture', original)[1], ['Unit/Case'])
        self.assertFalse(Path(observed[0]['HOME']).exists())
        self.assertFalse(Path(observed[0]['TMPDIR']).exists())
        self.assertEqual(original['HOME'], '/private/home')

    def test_identity_helper_belongs_to_the_selected_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / 'build'
            prefix = Path(directory) / 'installed'
            binary = build / 'examples/ndnsf-identity-check'
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\nexit 0\n')
            binary.chmod(0o755)
            target = {'facilities': ['identity-check']}
            original = {'NDNSF_IDENTITY_CHECK_EXECUTABLE': '/unrelated/helper'}
            selected = checks.target_environment(target, original, None, build)
            self.assertEqual(selected['NDNSF_IDENTITY_CHECK_EXECUTABLE'], str(binary))
            self.assertEqual(original['NDNSF_IDENTITY_CHECK_EXECUTABLE'], '/unrelated/helper')
            with self.assertRaisesRegex(ValueError, 'IDENTITY_CHECK_TEST_INPUT_MISSING'):
                checks.target_environment(target, original, prefix, build)

    def test_repo_cli_belongs_to_the_selected_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / 'build'
            prefix = Path(directory) / 'installed'
            binary = build / 'NDNSF-DistributedRepo/ndnsf-repo-owner-delete'
            binary.parent.mkdir(parents=True)
            binary.write_text('#!/bin/sh\nexit 0\n')
            binary.chmod(0o755)
            target = {'facilities': ['repo-owner-delete']}
            original = {'NDNSF_REPO_OWNER_DELETE_BIN': '/unrelated/helper'}
            selected = checks.target_environment(target, original, None, build)
            self.assertEqual(selected['NDNSF_REPO_OWNER_DELETE_BIN'], str(binary))
            self.assertEqual(original['NDNSF_REPO_OWNER_DELETE_BIN'], '/unrelated/helper')
            with self.assertRaisesRegex(ValueError, 'REPO_OWNER_DELETE_TEST_INPUT_MISSING'):
                checks.target_environment(target, original, prefix, build)

    def test_vendor_test_libraries_are_scoped_and_never_fall_back_to_source(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / 'build'
            prefix = Path(directory) / 'installed'
            root, files = checks.gpu_query_files(None, build)
            for path in files:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'owned test input')
                path.chmod(0o755)
            target = {'facilities': ['gpu-query-fixtures']}
            original = {'LD_LIBRARY_PATH': '/candidate/lib', 'SPEC217_GPU_FIXTURE_SCENARIO': 'bad'}
            selected = checks.target_environment(target, original, None, build)
            self.assertEqual(selected['LD_LIBRARY_PATH'], str(root) + ':/candidate/lib')
            self.assertEqual(selected['NDNSF_DEVICE_QUERY_EXECUTABLE'], str(files[-1]))
            self.assertNotIn('SPEC217_GPU_FIXTURE_SCENARIO', selected)
            self.assertEqual(original['LD_LIBRARY_PATH'], '/candidate/lib')
            self.assertEqual(checks.target_environment({}, original, None, build), original)
            with self.assertRaisesRegex(ValueError, 'GPU_QUERY_TEST_INPUT_MISSING'):
                checks.target_environment(target, original, prefix, build)
            files[0].unlink()
            with self.assertRaisesRegex(ValueError, 'GPU_QUERY_TEST_INPUT_MISSING'):
                checks.target_environment(target, original, None, build)

    def test_installed_receipt_rejects_missing_and_changed_delivery_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            roster = {'schema': 'ndnsf-check-profiles-v1',
                      'configuredComponents': ['core'], 'fixtureFiles': ['sample.txt'],
                      'profiles': {'unit': {'targets': [{'target': 'unit-tests'}]}}}
            names = ['bin/ndnsf-checks', 'share/ndnsf/tests/run-checks.py',
                     'share/ndnsf/tests/check-profiles.json', 'libexec/ndnsf-tests/unit-tests',
                     'share/ndnsf/tests/fixtures/sample.txt', 'lib/libndn-service-framework.so']
            records = {}
            for name in names:
                path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'original fixture bytes')
                path.chmod(0o644)
                records[str(path)] = {'owners': ['ndnsf-native'], 'installed': {
                    'kind': 'file', 'mode': 0o644,
                    'sha256': hashlib.sha256(b'original fixture bytes').hexdigest()}}
            manifest = prefix / 'share/ndnsf/stack-install-manifest.json'
            manifest.write_text(json.dumps({'schema': 1, 'files': records}))
            self.assertEqual(checks.verify_installed_files(prefix, roster, ['unit']), 6)
            for name in ['share/ndnsf/tests/fixtures/sample.txt',
                         'lib/libndn-service-framework.so']:
                path = prefix / name
                with self.subTest(changed=name):
                    path.write_bytes(b'replaced')
                    with self.assertRaisesRegex(ValueError, 'INSTALLED_FILE_MISMATCH'):
                        checks.verify_installed_files(prefix, roster, ['unit'])
                    path.write_bytes(b'original fixture bytes')
            (prefix / 'libexec/ndnsf-tests/unit-tests').unlink()
            with self.assertRaises(FileNotFoundError):
                checks.verify_installed_files(prefix, roster, ['unit'])

    def test_installed_mode_refuses_fixture_root_outside_the_verified_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            roster = {'schema': 'ndnsf-check-profiles-v1',
                      'configuredComponents': ['core'], 'fixtureFiles': ['sample.txt'],
                      'profiles': {'unit': {'targets': [{'target': 'unit-tests'}]}}}
            names = ['bin/ndnsf-checks', 'share/ndnsf/tests/run-checks.py',
                     'share/ndnsf/tests/check-profiles.json', 'libexec/ndnsf-tests/unit-tests',
                     'share/ndnsf/tests/fixtures/sample.txt', 'lib/libndn-service-framework.so']
            records = {}
            for name in names:
                path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'original fixture bytes')
                path.chmod(0o644)
                records[str(path)] = {'owners': ['ndnsf-native'], 'installed': {
                    'kind': 'file', 'mode': 0o644,
                    'sha256': hashlib.sha256(b'original fixture bytes').hexdigest()}}
            (prefix / 'share/ndnsf/stack-install-manifest.json').write_text(
                json.dumps({'schema': 1, 'files': records}))
            roster_path = prefix / 'share/ndnsf/tests/check-profiles.json'
            roster_path.write_text(json.dumps(roster))
            output = io.StringIO()
            outside = Path(directory).parent / 'outside-fixtures'
            with patch.object(checks, 'installed_prefix', return_value=prefix), \
                    contextlib.redirect_stderr(output):
                result = checks.main(['--profile', 'unit', '--fixture-root', str(outside)])
            # The receipt verifies the prefix's own root, so a different one is refused before any
            # binary is enumerated or started.
            self.assertEqual(result, checks.EXIT_ROSTER)
            self.assertIn('FIXTURE_ROOT_OUTSIDE_VERIFIED_PREFIX', output.getvalue())

    def test_stale_worker_tool_alias_does_not_satisfy_the_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            roster = {'schema': 'ndnsf-check-profiles-v1',
                      'configuredComponents': ['di'], 'fixtureFiles': ['sample.txt'],
                      'driverFiles': ['run-driver.py'],
                      'profiles': {'unit': {'targets': [{'target': 'unit-tests'}]}}}
            names = ['bin/ndnsf-checks', 'share/ndnsf/tests/run-checks.py',
                     'share/ndnsf/tests/check-profiles.json', 'libexec/ndnsf-tests/unit-tests',
                     'share/ndnsf/tests/fixtures/sample.txt',
                     'lib/libndn-service-framework.so', 'lib/libndnsf-distributed-inference.so',
                     'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker',
                     'share/ndnsf/tests/standalone/run-driver.py']
            # Only the retired alias is present: the manifest must still reject the prefix.
            names += ['libexec/ndnsf-tests/spec182-worker-tool-' + role
                      for role in ('block', 'sigkill', 'silent0', 'silent7', 'garbage')]
            records = {}
            for name in names:
                path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'worker')
                path.chmod(0o755)
                records[str(path)] = {'owners': ['ndnsf-native'], 'installed': {
                    'kind': 'file', 'mode': 0o755,
                    'sha256': hashlib.sha256(b'worker').hexdigest()}}
            (prefix / 'share/ndnsf/stack-install-manifest.json').write_text(
                json.dumps({'schema': 1, 'files': records}))
            with self.assertRaisesRegex(ValueError, 'INSTALL_MANIFEST_ENTRY_MISSING|worker-tool-'):
                checks.verify_installed_files(prefix, roster, ['unit'])

    def test_missing_build_roster_never_falls_back_to_checkout_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            output = io.StringIO()
            with patch.object(checks, 'installed_prefix', return_value=None), \
                    contextlib.redirect_stderr(output):
                result = checks.main(['--profile', 'unit', '--build-root', directory])
            self.assertEqual(result, checks.EXIT_ROSTER)
            self.assertIn('CONFIGURED_TEST_ROSTER_UNAVAILABLE', output.getvalue())

    def test_explicit_unavailable_component_profile_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'tests/check-profiles.json'
            path.parent.mkdir()
            path.write_text(json.dumps({'schema': 'ndnsf-check-profiles-v1',
                'configuredComponents': ['core'], 'profiles': {'model': {'targets': []}}}))
            output = io.StringIO()
            with patch.object(checks, 'installed_prefix', return_value=None), \
                    contextlib.redirect_stderr(output):
                result = checks.main(['--profile', 'model', '--build-root', directory])
            self.assertEqual(result, checks.EXIT_ROSTER)
            self.assertIn('CHECK_PROFILE_COMPONENT_UNAVAILABLE', output.getvalue())


class CheckRunner(unittest.TestCase):
    def test_listing_keeps_nested_identity_and_disabled_cases(self):
        listing = 'A*\n    Nested*\n        Same*\nB*\n    Same*\n    Optional \n'
        with patch.object(checks, 'native_process', return_value=Namespace(
                returncode=0, stdout='', stderr=listing, boundary=None)):
            suites, cases, disabled = checks.enumerate_cases('/fixture', {})
        self.assertEqual(suites, ['A', 'A/Nested', 'B'])
        self.assertEqual(cases, ['A/Nested/Same', 'B/Same', 'B/Optional'])
        self.assertEqual(disabled, ['B/Optional'])

    def test_exclusion_and_disabled_are_not_selected(self):
        target = {'target': 'fixture', 'selector': {'include': ['*'],
                                                   'excludeSuites': ['Model']}}
        cases = ['Unit/Same', 'Model/Same', 'Gpu/Optional']
        self.assertEqual(checks.select_cases(target, cases, ['Gpu/Optional']), ['Unit/Same'])

    def test_unknown_selector_is_an_error(self):
        with self.assertRaisesRegex(ValueError, 'CHECK_SELECTOR_UNMATCHED'):
            checks.select_cases({'target': 'fixture', 'selector': {'include': ['Missing/*']}},
                                ['Unit/Case'])

    def test_every_enabled_case_needs_a_declared_profile(self):
        roster = {'profiles': {'unit': {'targets': [
            {'target': 'fixture', 'selector': {'include': ['Unit/*']}}]}}}
        with self.assertRaisesRegex(ValueError, 'CHECK_CASE_UNOWNED.*Model/Case'):
            checks.case_owners(roster, 'fixture', ['Unit/Case', 'Model/Case'])

    def test_model_profile_owns_unit_exclusion_without_running_the_model(self):
        roster = {'profiles': {
            'unit': {'targets': [{'target': 'fixture',
                'selector': {'include': ['*'], 'excludeSuites': ['Model']}}]},
            'model': {'targets': [{'target': 'fixture',
                'selector': {'include': ['Model/*']}}]}}}
        owners = checks.case_owners(roster, 'fixture', ['Unit/Case', 'Model/Case'])
        self.assertEqual(owners, {'Unit/Case': ['unit'], 'Model/Case': ['model']})

    def test_xml_preserves_case_result_instead_of_assertion_count(self):
        with tempfile.TemporaryDirectory() as root:
            p = Path(root) / 'report.xml'
            p.write_text('<TestResult><TestSuite name="module" result="failed">'
                         '<TestSuite name="A"><TestCase name="Same" result="failed" '
                         'assertions_failed="7"/></TestSuite>'
                         '<TestSuite name="B"><TestCase name="Same" result="skipped"/>'
                         '</TestSuite></TestSuite></TestResult>')
            self.assertEqual(checks.read_case_results(p), {'A/Same': 'failed', 'B/Same': 'skipped'})

    def invoke(self, outcome='passed', exit_code=0, timeout=False, omit=False, message='',
               report=None, include=None, environment=None, observed=None):
        seen = []
        def process(argv, **kwargs):
            seen.append(argv)
            if '--list_content' in argv:
                return Namespace(returncode=0, stdout='', stderr='Unit*\n    Case*\nModel*\n    Case*\n')
            if timeout:
                raise subprocess.TimeoutExpired(argv, 1, output=b'Running 1 test case...')
            if not omit:
                output = Path(next(x.split('=', 1)[1] for x in argv if x.startswith('--report_sink=')))
                output.write_text('<TestResult><TestSuite name="module"><TestSuite name="Unit">'
                                  '<TestCase name="Case" result="%s" assertions_failed="7"/>'
                                  '</TestSuite></TestSuite></TestResult>' % outcome)
            return Namespace(returncode=exit_code, stdout='Running 1 test case...' + message, stderr='')
        target = {'target': 'fixture', 'selector': {
            'include': include or ['*'], 'excludeSuites': ['Model']}}
        def native(argv, environ, timeout, *, cwd=None):
            if observed is not None:
                observed.append(dict(environ, homeExists=Path(environ['HOME']).is_dir(),
                    spoolExists=Path(environ['NDNSF_REQUEST_LARGE_DATA_DIR']).is_dir(), cwd=cwd))
            return process(argv)
        with patch.object(checks.subprocess, 'run', side_effect=process), \
             patch.object(checks, 'native_process', side_effect=native):
            result = checks.run_target('/fixture', target, environment or {}, 1,
                                       Namespace(report=report))[0]
        self.assertIn('--run_test=!Model', seen[-1])
        return result

    def test_native_pass_requires_complete_case_report(self):
        result = self.invoke()
        self.assertTrue(result['passed'])
        self.assertEqual((result['selected'], result['executed'], result['failed']), (1, 1, 0))

    def test_not_run_message_cannot_be_reported_as_native_qualification(self):
        result = self.invoke(message='\nNOT_RUN: frozen export is absent\n')
        self.assertEqual(result['nativeExit'], 0)
        self.assertFalse(result['passed'])
        self.assertIn('NATIVE_NOT_RUN_MARKER', result['reportError'])

    def test_boost_zero_assertion_warning_cannot_qualify_a_returned_case(self):
        result = self.invoke(message='\nTest case Case did not check any assertions\n')
        self.assertEqual(result['nativeExit'], 0)
        self.assertFalse(result['passed'])
        self.assertIn('NATIVE_ASSERTION_FREE_CASE', result['reportError'])

    def test_selectors_sharing_one_binary_preserve_both_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            report = str(Path(directory) / 'report.json')
            first = self.invoke(message='\nfirst invocation\n', report=report)
            second = self.invoke(message='\nsecond invocation\n', report=report, include=['Unit/*'])
            self.assertNotEqual(first['log'], second['log'])
            self.assertIn('first invocation', Path(first['log']).read_text())
            self.assertNotIn('second invocation', Path(first['log']).read_text())
            self.assertIn('second invocation', Path(second['log']).read_text())

    def test_native_invocation_owns_home_and_publication_spool(self):
        original = {'HOME': '/private/home', 'NDN_CLIENT_PIB': 'private-pib',
                    'NDN_CLIENT_TPM': 'private-tpm',
                    'NDNSF_CONTROLLER_CERT_FILE': '/private/controller.cert',
                    'NDNSF_REQUEST_LARGE_DATA_DIR': '/shared/spool'}
        observed = []
        self.assertTrue(self.invoke(environment=original, observed=observed)['passed'])
        self.assertTrue(observed[0]['homeExists'])
        self.assertTrue(observed[0]['spoolExists'])
        self.assertEqual(Path(observed[0]['cwd']).parent, Path(observed[0]['HOME']).parent)
        self.assertFalse(Path(observed[0]['cwd']).exists())
        self.assertNotIn('NDN_CLIENT_PIB', observed[0])
        self.assertNotIn('NDN_CLIENT_TPM', observed[0])
        self.assertNotIn('NDNSF_CONTROLLER_CERT_FILE', observed[0])
        self.assertFalse(Path(observed[0]['HOME']).exists())
        self.assertFalse(Path(observed[0]['NDNSF_REQUEST_LARGE_DATA_DIR']).exists())
        self.assertEqual(original['HOME'], '/private/home')
        self.assertEqual(original['NDNSF_REQUEST_LARGE_DATA_DIR'], '/shared/spool')

    def test_failures_count_cases_not_assertions(self):
        result = self.invoke('failed', 201)
        self.assertFalse(result['passed'])
        self.assertEqual((result['status'], result['failed']), (201, 1))

    def test_native_skip_is_not_a_pass(self):
        result = self.invoke('skipped')
        self.assertFalse(result['passed'])
        self.assertEqual((result['executed'], result['skipped']), (0, 1))

    def test_missing_report_and_timeout_are_not_passes(self):
        for kwargs in ({'omit': True}, {'timeout': True}):
            with self.subTest(kwargs=kwargs):
                result = self.invoke(**kwargs)
                self.assertFalse(result['passed'])
                self.assertEqual(result['unavailable'], 1)
                self.assertIsNotNone(result['reportError'])

    def test_native_output_is_bounded(self):
        with patch.object(checks, 'MAX_OUTPUT_BYTES', 4096):
            result = checks.native_process([sys.executable, '-c',
                'import os; os.write(1, b"x"*1000000)'], os.environ.copy(), 5)
        self.assertEqual(result.returncode, 125)
        self.assertLessEqual(len(result.stdout.encode()) + len(result.stderr.encode()), 4096)

    def test_native_deadline_cancels_descendants_with_inherited_pipes(self):
        result = checks.native_process([sys.executable, '-c',
            'import subprocess,sys; p=subprocess.Popen([sys.executable,"-c",'
            '"import time; time.sleep(30)"]); print(p.pid,flush=True)'],
            os.environ.copy(), .2)
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.boundary, 'NATIVE_TIMEOUT')
        pid = int(result.stdout.strip())
        state = Path('/proc/%d/stat' % pid)
        self.assertTrue(not state.exists() or state.read_text().split()[2] == 'Z')

    def test_leftover_detached_output_child_cannot_be_a_pass(self):
        result = checks.native_process([sys.executable, '-c',
            'import subprocess,sys; subprocess.Popen([sys.executable,"-c",'
            '"import time; time.sleep(30)"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)'],
            os.environ.copy(), 5)
        self.assertEqual(result.returncode, 125)
        self.assertEqual(result.nativeExit, 0)
        self.assertEqual(result.boundary, 'NATIVE_DESCENDANTS_NOT_DRAINED')

    def test_new_process_group_still_belongs_to_the_owned_session(self):
        script = ('import os,subprocess,sys; '
                  'p=subprocess.Popen([sys.executable,"-c",'
                  '"import os,time; os.setpgrp(); print(1,flush=True); time.sleep(30)"],'
                  'stdout=subprocess.PIPE,stderr=subprocess.DEVNULL); '
                  'p.stdout.readline(); print(p.pid,os.getsid(0),flush=True)')
        result = checks.native_process([sys.executable, '-c', script], os.environ.copy(), 5)
        pid, session = map(int, result.stdout.split())
        try:
            self.assertEqual(result.returncode, 125)
            self.assertEqual(result.nativeExit, 0)
            self.assertEqual(result.boundary, 'NATIVE_DESCENDANTS_NOT_DRAINED')
            state = Path('/proc/%d/stat' % pid)
            self.assertTrue(not state.exists() or state.read_text().split()[2] == 'Z')
        finally:
            try:
                if os.getsid(pid) == session:
                    os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


if __name__ == '__main__':
    unittest.main()
