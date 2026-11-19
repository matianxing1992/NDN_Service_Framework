"""Component selection for the maintained native test registrations and their roster."""
import copy
import json


COMPONENT_ROOTS = {
    'NDNSF-DistributedInference/': 'di',
    'NDNSF-DistributedRepo/': 'repo',
    'NDNSF-UAV-APP/': 'uav',
}
COMPONENT_LIBRARIES = {
    'ndnsf-distributed-inference': 'di',
    'ndnsf-distributed-repo': 'repo',
}
EXTERNAL_LIBRARIES = {
    'ONNX': 'di', 'ONNXRUNTIME': 'di', 'TOKENIZER_BRIDGE': 'di',
    'GSTREAMER': 'uav', 'GMODULE': 'uav', 'FFI': 'uav', 'PCRE': 'uav', 'gtkmm': 'uav',
}
CANONICAL_TARGETS = {'unit-tests', 'integration-tests'}


def source_component(path, catalog):
    """Classify an actual source; new test sources require an explicit inventory entry."""
    if path.startswith('tests/') and path.endswith(('.cpp', '.cc', '.c')):
        if path not in catalog:
            raise ValueError('TEST_SOURCE_COMPONENT_UNDECLARED: ' + path)
        return catalog[path]
    for root, component in COMPONENT_ROOTS.items():
        if path.startswith(root):
            return component
    if path.startswith(('examples/DI_', 'examples/spec220-onnx-reference-oracle')):
        return 'di'
    return 'core'


def configured_roster(roster, components):
    result = copy.deepcopy(roster)
    result['configuredComponents'] = list(components)
    for profile in result['profiles'].values():
        profile['targets'] = [entry for entry in profile['targets']
                              if set(entry.get('requiresComponents', [])) <= set(components)]
        if 'repo' not in components:
            for entry in profile['targets']:
                if 'repo-owner-delete' in entry.get('facilities', []):
                    entry['facilities'].remove('repo-owner-delete')
    return result


def write_roster(task):
    roster = json.loads(task.inputs[0].read())
    selected = configured_roster(roster, task.env.NDNSF_COMPONENTS)
    selected['fixtureFiles'] = list(task.env.NDNSF_TEST_FIXTURE_FILES)
    selected['driverFiles'] = list(task.env.NDNSF_TEST_DRIVER_FILES)
    task.outputs[0].write(json.dumps(selected,
                                    indent=2, sort_keys=True) + '\n')
    return 0


def apply_component_scope(bld, roster):
    """Filter before Waf posts tasks, preserving task indices and the canonical owners."""
    components = set(bld.env.NDNSF_COMPONENTS)
    catalog = roster['sourceComponents']
    if not set(catalog.values()) <= {'core', 'repo', 'di', 'uav'}:
        raise ValueError('TEST_SOURCE_COMPONENT_INVALID')
    reserved_indices = {generator.idx for group in bld.groups for generator in group
                        if generator.path == bld.path and generator.name in CANONICAL_TARGETS}
    object_owners = {}
    for group in bld.groups:
        for generator in list(group):
            native_features = {'cxxprogram', 'cxxshlib', 'cxxstlib', 'cprogram', 'cshlib', 'cstlib'}
            if (generator.path != bld.path or
                    not native_features.intersection(generator.to_list(generator.features))):
                continue
            if generator.name not in CANONICAL_TARGETS and generator.idx in reserved_indices:
                # A shorter component/resource list can otherwise assign this reserved namespace
                # to a small focused target. Keep its outputs separate from the umbrellas.
                generator.idx += 10000
            if generator.idx in object_owners:
                raise ValueError('TEST_OBJECT_INDEX_COLLISION: %s and %s' %
                                 (object_owners[generator.idx], generator.name))
            object_owners[generator.idx] = generator.name
            sources = generator.to_nodes(generator.source)
            owners = {node: source_component(node.path_from(bld.srcnode), catalog)
                      for node in sources}
            canonical = generator.name in CANONICAL_TARGETS
            libraries = generator.to_list(getattr(generator, 'use', [])) + \
                generator.to_list(getattr(generator, 'lib', []))
            required = set(owners.values()) | {
                COMPONENT_LIBRARIES[lib] for lib in libraries
                if lib in COMPONENT_LIBRARIES}
            flags = generator.to_list(getattr(generator, 'linkflags', []))
            sanitizer_only = (any(flag.startswith('-fsanitize=') and
                                  {'address', 'undefined'}.intersection(flag[11:].split(','))
                                  for flag in flags) and
                              not bld.env.sanitize_di and bld.bldnode.name == 'build')
            if sanitizer_only or (not canonical and not required <= components):
                group.remove(generator)
                bld.task_gen_cache_names.pop(generator.name, None)
                continue
            if canonical:
                generator.source = [node for node in sources if owners[node] in components]
            for attribute in ('use', 'lib'):
                setattr(generator, attribute,
                        [lib for lib in generator.to_list(getattr(generator, attribute, []))
                         if COMPONENT_LIBRARIES.get(lib,
                            EXTERNAL_LIBRARIES.get(lib, 'core')) in components])
            defines = generator.to_list(getattr(generator, 'defines', []))
            if not any(value.startswith('NDNSF_TEST_SOURCE_DIR=') for value in defines):
                defines += ['NDNSF_TEST_SOURCE_DIR="%s"' % bld.srcnode.abspath()]
            if not any(value.startswith('NDNSF_TEST_BINARY_DIR=') for value in defines):
                defines += ['NDNSF_TEST_BINARY_DIR="%s"' % bld.bldnode.abspath()]
            generator.defines = defines
            # The generated roster must accompany either canonical binary even for --targets.
            if canonical:
                generator.use = generator.to_list(generator.use) + ['ndnsf-test-roster']
