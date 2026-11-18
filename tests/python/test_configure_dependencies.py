"""Isolated configure inventory tests: no apt, network, or native build."""
import importlib.util
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'configure_dependencies', ROOT / 'scripts/configure_dependencies.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class NixIdentityTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        store = Path(self.directory.name) / 'store'
        store.mkdir()
        self.addCleanup(inventory.load_nix_build_identity.cache_clear)
        patched = patch.object(inventory, 'NIX_STORE', store)
        patched.start()
        self.addCleanup(patched.stop)
        tools = store / ('a' * 32 + '-tools')
        deps = store / ('b' * 32 + '-deps')
        (tools / 'bin').mkdir(parents=True)
        (deps / 'lib').mkdir(parents=True)
        (deps / 'include/boost').mkdir(parents=True)
        (deps / 'include/boost/version.hpp').write_text('#define BOOST_VERSION 107100\n')
        for name in ('ld', 'ar', 'ranlib', 'nm', 'cc1plus'):
            shutil.copy2('/bin/true', tools / 'bin' / name)
        shutil.copy2(shutil.which('readelf'), tools / 'bin/readelf')
        compiler = tools / 'bin/g++'
        compiler.write_text('#!/bin/sh\n'
                            'if [ "$1" = "-dumpmachine" ]; then echo x86_64-test-linux; exit; fi\n'
                            'for arg do case "$arg" in -print-prog-name=*) '
                            f'printf "%s/%s\\n" "{tools}/bin" "${{arg#*=}}"; exit;; esac; done\n')
        compiler.chmod(0o755)
        self.library = deps / 'lib/libfixture.so'
        subprocess.run(['/usr/bin/cc', '-shared', '-fPIC', '-Wl,-soname,libfixture.so',
                        '-x', 'c', '-', '-o', str(self.library)],
                       input='int fixture(void) { return 1; }\n', text=True, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.identity = inventory.make_nix_build_identity({
            'roots': [str(tools), str(deps)], 'compiler': str(compiler),
            'binutilsPrefix': str(tools / 'bin'), 'readelf': str(tools / 'bin/readelf'),
            'libraries': {'libfixture.so': str(self.library)},
            'boost': {'includes': str(deps / 'include'), 'libraries': str(deps / 'lib'),
                      'version': 107100, 'soname': '1.71.0'}})
        self.receipt = store / ('c' * 32 + '-identity.json')
        self.receipt.write_text(json.dumps(self.identity))

    def test_exact_identity_and_declared_compiler_are_accepted(self):
        self.assertEqual(inventory.load_nix_build_identity(str(self.receipt)), self.identity)
        inventory.validate_nix_compiler(self.identity, self.identity['tools']['cxx']['path'])

    def test_bare_store_and_host_paths_are_not_dependency_roots(self):
        changed = copy.deepcopy(self.identity)
        changed['roots'] = [str(inventory.NIX_STORE)]
        with self.assertRaisesRegex(ValueError, 'exact immutable store'):
            inventory.validate_nix_build_identity(changed)
        with self.assertRaisesRegex(ValueError, 'undeclared Nix dependency'):
            inventory.require_nix_path(self.identity, '/usr/include')

    def test_changed_library_digest_or_abi_is_rejected(self):
        for key, value, reason in [('sha256', '0' * 64, 'digest/path'),
                                   ('soname', 'wrong.so', 'ABI/SONAME'),
                                   ('elfClass', 'ELF32', 'ABI/SONAME')]:
            with self.subTest(key=key):
                changed = copy.deepcopy(self.identity)
                changed['libraries']['libfixture.so'][key] = value
                with self.assertRaisesRegex(ValueError, reason):
                    inventory.validate_nix_build_identity(changed)

    def test_symlink_escape_and_foreign_compiler_are_rejected(self):
        self.library.unlink()
        self.library.symlink_to('/bin/true')
        with self.assertRaisesRegex(ValueError, 'undeclared Nix dependency'):
            inventory.validate_nix_build_identity(self.identity)
        with self.assertRaisesRegex(ValueError, 'differs from the declared toolchain'):
            inventory.validate_nix_compiler(self.identity, '/bin/true')

    def test_other_declared_output_cannot_replace_the_selected_library(self):
        other = Path(self.identity['roots'][0]) / 'lib'
        other.mkdir()
        shutil.copy2(self.library, other / 'libfixture.so')
        with self.assertRaisesRegex(ValueError, 'Selected library differs'):
            inventory.validate_nix_library_selection(
                self.identity, [str(other), str(self.library.parent)], ['fixture'])

    def test_declared_tool_bytes_do_not_authorize_a_different_linker_selection(self):
        changed = copy.deepcopy(self.identity)
        changed['tools']['ld'] = copy.deepcopy(changed['tools']['ar'])
        with self.assertRaisesRegex(ValueError, 'resolved a different tool: ld'):
            inventory.validate_nix_compiler(changed, changed['tools']['cxx']['path'])

    def test_compiler_and_global_flags_cannot_override_the_declared_tools(self):
        for flag in ('-B/other/bin/', '-specs=other.specs', '-fuse-ld=lld'):
            with self.subTest(flag=flag):
                with self.assertRaisesRegex(ValueError, 'undeclared arguments'):
                    inventory.validate_nix_compiler(
                        self.identity, [self.identity['tools']['cxx']['path'], flag])
                with self.assertRaisesRegex(ValueError, 'toolchain override'):
                    inventory.reject_nix_tool_overrides([flag])

    def test_an_executable_cannot_masquerade_as_a_digest_matched_library(self):
        executable = self.library.with_name('libopenabe.so')
        shutil.copyfile('/bin/true', executable)
        self.identity['libraries'] = {'libopenabe.so': {
            **inventory._file_identity(executable),
            **inventory._elf_identity(self.identity['tools']['readelf']['path'], executable)}}
        with self.assertRaisesRegex(ValueError, 'ABI/SONAME'):
            inventory.validate_nix_build_identity(self.identity)

    def test_environment_roots_must_equal_the_immutable_receipt(self):
        with patch.dict(os.environ, {'NDNSF_NIX_BUILD_IDENTITY': str(self.receipt),
                                     'NDNSF_NIX_DEPENDENCY_ROOTS': '',
                                     'NDNSF_CONTAINER_BUILD': '0'}):
            self.assertEqual(inventory.nix_build_identity(), self.identity)
            os.environ['NDNSF_NIX_DEPENDENCY_ROOTS'] = str(inventory.NIX_STORE)
            with self.assertRaisesRegex(ValueError, 'differ from the identity receipt'):
                inventory.nix_build_identity()

    def test_container_marker_or_unreceipted_roots_cannot_select_nix(self):
        with patch.dict(os.environ, {'NDNSF_NIX_BUILD_IDENTITY': '',
                                     'NDNSF_NIX_DEPENDENCY_ROOTS': str(inventory.NIX_STORE)}):
            with self.assertRaisesRegex(ValueError, 'validated Nix build identity'):
                inventory.nix_build_identity()
        with patch.dict(os.environ, {'NDNSF_NIX_BUILD_IDENTITY': str(self.receipt),
                                     'NDNSF_CONTAINER_BUILD': '1'}):
            with self.assertRaisesRegex(ValueError, 'cannot use the container marker'):
                inventory.nix_build_identity()

class ConfigureDependenciesTest(unittest.TestCase):
    def test_core_and_repo_inventory_do_not_require_model_or_gui_sdks(self):
        for profile in ('core', 'repo'):
            with self.subTest(profile=profile), \
                 patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
                 patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=0)) as run, \
                 patch.object(inventory.Path, 'is_file', autospec=True,
                              side_effect=lambda p: 'nac-abe' in str(p)):
                self.assertEqual(inventory.check_dependencies(components=profile), ([], []))
            queried = ' '.join(str(call.args) for call in run.call_args_list)
            self.assertNotIn('onnx', queried)
            self.assertNotIn('gtk', queried)
            self.assertNotIn('gstreamer', queried)

    def test_di_requires_model_sdk_but_not_gui(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)) as run, \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies(components='di')
        self.assertTrue(any('onnxruntime' in e for e in errors))
        self.assertTrue(any('tokenizer_bridge' in e for e in errors))
        self.assertFalse(any('gtk' in str(call.args) for call in run.call_args_list))
        self.assertEqual(warnings, [])

    def test_core_sdk_only_requires_no_model_tools(self):
        with patch.object(inventory.shutil, 'which', return_value=None), \
             patch.object(inventory.subprocess, 'run') as run:
            self.assertEqual(inventory.check_dependencies(components='core', sdk_only=True), ([], []))
        run.assert_not_called()

    def test_unknown_component_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'components must be'):
            inventory.check_dependencies(components='unknown')

    def test_aggregates_missing_metadata_and_files(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)), \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies()
        self.assertTrue(any('ndn-cxx' in e for e in errors))
        self.assertTrue(any('onnxruntime' in e for e in errors))
        self.assertTrue(any('tokenizer' in e for e in errors))
        self.assertTrue(warnings)

    def test_missing_tools_do_not_attempt_pkg_config(self):
        with patch.object(inventory.shutil, 'which', return_value=None), \
             patch.object(inventory.subprocess, 'run') as run, \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, _ = inventory.check_dependencies()
        run.assert_not_called()
        self.assertTrue(any('protoc' in e for e in errors))

    def test_installed_inventory(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=0)), \
             patch.object(inventory.Path, 'is_file', return_value=True):
            self.assertEqual(inventory.check_dependencies(), ([], []))

    def test_sdk_only_reports_sdk_not_ndn_or_gui(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory.subprocess, 'run', return_value=Mock(returncode=1)), \
             patch.object(inventory.Path, 'is_file', return_value=False):
            errors, warnings = inventory.check_dependencies(sdk_only=True)
        text = '\n'.join(errors)
        self.assertIn('onnxruntime', text)
        self.assertIn('tokenizer_bridge', text)
        self.assertNotIn('nac-abe', text)
        self.assertNotIn('gtkmm', text)
        self.assertEqual(warnings, [])

    def test_script_dry_run_is_non_mutating(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--dry-run',
                                 '--', '--with-tests'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--with-tests', result.stdout)
        self.assertIn('Configure-only mode requires ONNX Runtime', result.stdout)
        self.assertIn('no apt, git fetch, builds, installation or Waf execution', result.stdout)

    def test_unknown_option_rejected(self):
        result = subprocess.run(['bash', str(ROOT / 'configure.sh'), '--bogus'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
