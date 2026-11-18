"""Regressions against the configured production Waf graph; never compile."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
CONFIG = ROOT / 'build/c4che/_cache.py'
CONFIGURED_TEST_BUILD = ((ROOT / '.lock-waf_linux_build').is_file() and
                         CONFIG.is_file() and 'WITH_TESTS = True' in CONFIG.read_text())
SPEC = importlib.util.spec_from_file_location(
    'build_closure', ROOT / 'scripts/spec201-build-closure.py')
CLOSURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLOSURE)


def load_graph(targets, command='build'):
    return CLOSURE.load_build_graph(
        ROOT, ROOT / '.lock-waf_linux_build', targets, command)


def post_selected(build):
    build._min_grp, build._exact_tg = build.get_targets()
    for index in range(len(build.groups)):
        build.current_group = index
        build.post_group()


@unittest.skipUnless(CONFIGURED_TEST_BUILD, 'requires the configured native test build')
class IncrementalBuildGraph(unittest.TestCase):
    def test_sanitized_lifetime_probes_instrument_their_callers(self):
        lock = ROOT / 'build-ASAN-UBSAN/.lock-waf_linux_build'
        if not lock.is_file():
            self.skipTest('requires the configured sanitizer build')
        expected = {
            'di-provider-assembly': {'main.cpp', 'di-prepared-provider.t.cpp',
                                        'ndnsf-integration-fixture.cpp', 'Provider.cpp'},
            'di-runtime': {'main.cpp', 'di-runtime.t.cpp'},
        }
        build = CLOSURE.load_build_graph(ROOT, lock, list(expected), 'build')
        self.assertTrue(build.env.sanitize_di)
        post_selected(build)
        for name, sources in expected.items():
            probe = build.get_tgen_by_name(name)
            self.assertEqual({task.inputs[0].name for task in probe.compiled_tasks}, sources)
            for task in probe.compiled_tasks:
                self.assertIn('-fsanitize=address,undefined', task.env.CXXFLAGS)
            self.assertIn('-fsanitize=address,undefined', probe.link_task.env.LINKFLAGS)
        core = build.get_tgen_by_name('ndnsf-di-core-objects')
        self.assertTrue(core.compiled_tasks)
        self.assertTrue(all('-fsanitize=address,undefined' in task.env.CXXFLAGS
                            for task in core.compiled_tasks))

    def test_adding_a_fixture_preserves_native_object_paths(self):
        targets = ['unit-tests', 'integration-tests', 'spec224-tp-tests']

        def object_paths(build):
            post_selected(build)
            return {name: sorted(output.abspath()
                    for task in build.get_tgen_by_name(name).compiled_tasks
                    for output in task.outputs) for name in targets}

        original = object_paths(load_graph(targets))
        from waflib import Node
        original_glob = Node.Node.ant_glob

        def extra_fixture(node, *args, **kwargs):
            files = original_glob(node, *args, **kwargs)
            if node.abspath() == str(ROOT / 'tests') and args == ('fixtures/**/*',):
                # A graph-only node: no source file or build artifact is written.
                return files + [node.make_node('fixtures/object-index-probe.json')]
            return files

        with patch.object(Node.Node, 'ant_glob', extra_fixture):
            changed = object_paths(load_graph(targets))
        for name in targets:
            with self.subTest(target=name):
                self.assertTrue(original[name])
                self.assertEqual(original[name], changed[name])

    def test_production_di_has_one_compilation_owner(self):
        build = load_graph(['ndnsf-distributed-inference'])
        names = ['ndnsf-distributed-inference', 'ndnsf-di-core-objects',
                 'ndnsf-di-adapter-onnx-objects', 'ndnsf-di-adapter-yolo-objects',
                 'ndnsf-di-adapter-qwen-objects']
        owners = {}
        for name in names:
            generator = build.get_tgen_by_name(name)
            generator.post()
            for task in getattr(generator, 'compiled_tasks', []):
                owners.setdefault(task.inputs[0].abspath(), []).append(name)
        self.assertTrue(owners)
        self.assertEqual({source: value for source, value in owners.items()
                          if len(value) != 1}, {})

    def test_default_build_selects_runtime_not_test_suites(self):
        build = load_graph([])
        self.assertTrue(build.targets)
        post_selected(build)
        self.assertTrue(build.get_tgen_by_name('ndnsf-distributed-inference').posted)
        self.assertFalse(getattr(build.get_tgen_by_name('unit-tests'), 'posted', False))
        self.assertFalse(getattr(build.get_tgen_by_name('integration-tests'), 'posted', False))
        self.assertFalse(getattr(build.get_tgen_by_name('spec220-tensor-work-asan'),
                                 'posted', False))

    def test_normal_transport_tests_link_production_di(self):
        build = load_graph(['spec181-exact-tensor-transport',
                            'spec181-protected-runtime-closure'])
        post_selected(build)
        for name in build.targets.split(','):
            generator = build.get_tgen_by_name(name)
            self.assertIn('ndnsf-distributed-inference', generator.to_list(generator.use))
            self.assertFalse(any('/NDNSF-DistributedInference/cpp/' in
                                 task.inputs[0].abspath()
                                 for task in generator.compiled_tasks))

    def test_install_keeps_public_headers_and_pkg_config(self):
        build = load_graph([], 'install')
        post_selected(build)
        generators = [g for group in build.groups for g in group]
        installed = [g for g in generators
                     if 'install_task' in getattr(g, 'features', [])]
        self.assertTrue(installed)
        self.assertTrue(all(getattr(g, 'posted', False) for g in installed))
        packages = [g for g in generators
                    if str(getattr(g, 'target', '')).endswith('.pc')]
        self.assertEqual(len(packages), 2)
        self.assertTrue(all(getattr(g, 'posted', False) for g in packages))

    def test_explicit_install_keeps_pkg_config(self):
        build = load_graph(['ndnsf-distributed-inference'], 'install')
        post_selected(build)
        packages = [g for group in build.groups for g in group
                    if str(getattr(g, 'target', '')).endswith('.pc')]
        self.assertEqual(len(packages), 2)
        self.assertTrue(all(getattr(g, 'posted', False) for g in packages))

    def test_special_test_compilations_are_retained(self):
        build = load_graph(['spec190-live-turns', 'spec220-tensor-work-asan'])
        post_selected(build)
        seam = build.get_tgen_by_name('spec190-live-turns')
        handler = [t for t in seam.compiled_tasks
                   if t.inputs[0].name == 'NativeProviderHandler.cpp']
        self.assertEqual(len(handler), 1)
        self.assertIn('NDNSF_DI_PROVIDER_TEST_SEAM', handler[0].env.DEFINES)
        sanitizer = build.get_tgen_by_name('spec220-tensor-work-asan')
        self.assertTrue(any('-fsanitize=address,undefined' in t.env.CXXFLAGS
                            for t in sanitizer.compiled_tasks))


if __name__ == '__main__':
    unittest.main()
