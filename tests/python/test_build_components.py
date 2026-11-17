"""Build/runner selection contracts, without substituting for native execution."""
import importlib.util
import json
from pathlib import Path
import unittest
from types import SimpleNamespace

TESTS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('test_components', TESTS / 'build-components.py')
components = importlib.util.module_from_spec(spec)
spec.loader.exec_module(components)


class BuildComponentContractTest(unittest.TestCase):
    def test_component_filter_preserves_registered_focused_programs(self):
        for command, requested in (
                ('build', ''), ('install', ''),
                ('build', 'focused'), ('list', '')):
            with self.subTest(command=command, requested=requested):
                path = SimpleNamespace(abspath=lambda: '/fixture')
                def generator(name, index, install):
                    return SimpleNamespace(name=name, idx=index, path=path,
                        features=['cxxprogram'], install_path=install,
                        source=[], use=[], lib=[], defines=[], to_nodes=lambda value: value,
                        to_list=lambda value: value if isinstance(value, list) else [value])
                unit = generator('unit-tests', 309, '/candidate/tests')
                helper = generator('helper', 311, '/candidate/tests')
                focused = generator('focused', 312, None)
                bld = SimpleNamespace(path=path, srcnode=path, bldnode=path,
                    cmd=command, targets=requested,
                    env=SimpleNamespace(NDNSF_COMPONENTS=['core']),
                    groups=[[unit, helper, focused]], task_gen_cache_names={'focused': focused})
                components.apply_component_scope(bld, {'sourceComponents': {}})
                self.assertEqual([g.name for g in bld.groups[0]],
                                 ['unit-tests', 'helper', 'focused'])
                self.assertIn('focused', bld.task_gen_cache_names)

    def test_focused_target_cannot_overwrite_reserved_umbrella_objects(self):
        path = SimpleNamespace(abspath=lambda: '/fixture')
        def generator(name, index):
            return SimpleNamespace(name=name, idx=index, path=path, features=['cxxprogram'],
                source=[], use=[], lib=[], defines=[], to_nodes=lambda value: value,
                to_list=lambda value: value if isinstance(value, list) else [value])
        unit = generator('unit-tests', 309)
        integration = generator('integration-tests', 310)
        focused = generator('focused', 309)
        bld = SimpleNamespace(path=path, srcnode=path, bldnode=path,
            env=SimpleNamespace(NDNSF_COMPONENTS=['core']),
            groups=[[unit, integration, focused]], task_gen_cache_names={})
        components.apply_component_scope(bld, {'sourceComponents': {}})
        self.assertEqual((unit.idx, integration.idx), (309, 310))
        self.assertNotIn(focused.idx, (unit.idx, integration.idx))

    def test_native_sources_have_explicit_component_ownership(self):
        roster = json.loads((TESTS / 'check-profiles.json').read_text())
        catalog = roster['sourceComponents']
        for directory in ('unit-tests', 'integration-tests', 'standalone', 'fixtures'):
            for source in (TESTS / directory).rglob('*.cpp'):
                with self.subTest(source=source.name):
                    self.assertIn(source.relative_to(TESTS.parent).as_posix(), catalog)
        with self.assertRaisesRegex(ValueError, 'TEST_SOURCE_COMPONENT_UNDECLARED'):
            components.source_component('tests/unit-tests/undeclared.cpp', catalog)

    def test_core_roster_keeps_native_owners_and_refuses_model_profile(self):
        roster = json.loads((TESTS / 'check-profiles.json').read_text())
        selected = components.configured_roster(roster, ['core'])
        self.assertEqual([row['target'] for row in selected['profiles']['unit']['targets']],
                         ['unit-tests'])
        self.assertEqual([row['target'] for row in selected['profiles']['integration']['targets']],
                         ['integration-tests'])
        self.assertEqual(selected['profiles']['model']['targets'], [])
        self.assertNotIn('configuredComponents', roster)

    def test_di_roster_retains_unique_provider_cases_and_model_owner(self):
        roster = json.loads((TESTS / 'check-profiles.json').read_text())
        selected = components.configured_roster(roster, ['core', 'repo', 'di'])
        self.assertIn('spec185-provider-assembly',
                      [row['target'] for row in selected['profiles']['integration']['targets']])
        self.assertEqual(len(selected['profiles']['model']['targets']), 1)


if __name__ == '__main__':
    unittest.main()
