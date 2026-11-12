"""Regressions against the configured production Waf graph; never compile."""
import importlib.util
from pathlib import Path
import sys
import unittest


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
