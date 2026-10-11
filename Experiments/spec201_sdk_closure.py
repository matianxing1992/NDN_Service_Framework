"""Read-only proof for bindings compiled by the maintained setup.py flows."""
import hashlib
import ast
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess


def identity(path):
    path = Path(path).resolve(strict=True)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest.hexdigest()}


def collect_sdk_closure(*, wrapper, log, built, installed, environment,
                        exit_status, native_receipt=None):
    if exit_status != 0:
        raise RuntimeError('SPEC201_SDK_BUILD_FAILED')
    wrapper = Path(wrapper).resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    if wrapper not in (root / 'pythonWrapper', root / 'NDNSF-DistributedRepo/pythonWrapper'):
        raise RuntimeError('SPEC201_SDK_MAINTAINED_WRAPPER_REQUIRED')
    core = wrapper == root / 'pythonWrapper'
    package = 'ndnsf' if core else 'py_repoclient'
    installed = Path(installed).resolve(strict=True)
    built = Path(built).resolve(strict=True)
    if (installed.parent != Path('/usr/local/lib/python3.8/dist-packages') / package
            or installed == built
            or built.parent != wrapper / package):
        raise RuntimeError('SPEC201_SDK_INSTALLED_PACKAGE_MISMATCH')
    if core != bool(native_receipt):
        raise RuntimeError('SPEC201_SDK_NATIVE_RECEIPT_MISMATCH')
    setup = wrapper / 'setup.py'
    source_lists = [node.value for node in ast.walk(ast.parse(setup.read_text()))
                    if isinstance(node, ast.keyword) and node.arg == 'sources']
    if len(source_lists) != 1:
        raise RuntimeError('SPEC201_SDK_SOURCE_INVENTORY_AMBIGUOUS')
    expected_sources = {str((wrapper / source).resolve(strict=True))
                        for source in ast.literal_eval(source_lists[0])}
    rows, links = [], []
    for line in Path(log).read_text().splitlines():
        try:
            command = shlex.split(line)
        except ValueError:
            continue
        if not command or Path(command[0]).name not in ('gcc', 'g++', 'c++'):
            continue
        if '-o' not in command:
            continue
        compiler = shutil.which(command[0], path=environment.get('PATH', os.defpath))
        if compiler is None:
            raise RuntimeError('SPEC201_SDK_COMPILER_MISSING')
        output = wrapper / command[command.index('-o') + 1]
        if '-c' in command:
            source = wrapper / command[command.index('-c') + 1]
            dependency_file = output.with_suffix('.d')
            dependency_text = dependency_file.read_text().replace('\\\n', '')
            dependencies = [identity(wrapper / value) for value in
                            shlex.split(dependency_text.split(':', 1)[1])]
            if not dependencies or identity(source) not in dependencies:
                raise RuntimeError('SPEC201_SDK_SOURCE_DEPENDENCY_MISSING')
            rows.append({'command': command, 'compiler': identity(compiler),
                         'source': identity(source), 'dependencies': dependencies,
                         'dependencyFile': identity(dependency_file),
                         'output': identity(output)})
        elif '-shared' in command:
            links.append({'command': command, 'compiler': identity(compiler),
                          'inputs': [identity(wrapper / value) for value in command
                                     if value.endswith(('.a', '.so')) and not value.startswith('-')],
                          'output': identity(output)})
    if not rows or len(links) != 1 or links[0]['output']['sha256'] != identity(built)['sha256']:
        raise RuntimeError('SPEC201_SDK_COMPILE_LINK_PROOF_MISSING')
    if {row['source']['path'] for row in rows} != expected_sources:
        raise RuntimeError('SPEC201_SDK_SOURCE_INVENTORY_MISMATCH')
    return {'schema': 'ndnsf-spec201-sdk-closure-1', 'wrapper': str(wrapper),
            'packaging': [identity(setup), identity(wrapper / 'pyproject.toml')],
            'sourceGuards': [identity(path) for path in sorted(
                set(wrapper.rglob('*.py')) | set(wrapper.rglob('*.cpp')) |
                set((root / 'ndn-service-framework').rglob('*.hpp')) |
                set((root / 'NDNSF-DistributedInference/cpp/ndnsf-di').rglob('*.hpp')) |
                set((root / 'NDNSF-DistributedRepo/include').rglob('*.hpp')))
                if 'build' not in path.relative_to(root).parts],
            'log': identity(log), 'rows': rows, 'links': links,
            'built': identity(built), 'installed': identity(installed),
            'environment': environment, 'exitStatus': exit_status,
            'nativeReceipt': identity(native_receipt) if native_receipt else None}


def verify_sdk_closure(record, binary_check):
    if record.get('schema') != 'ndnsf-spec201-sdk-closure-1':
        raise RuntimeError('SPEC201_SDK_CLOSURE_SCHEMA_MISMATCH')
    if record.get('exitStatus') != 0:
        raise RuntimeError('SPEC201_SDK_BUILD_FAILED')
    wrapper = Path(record['wrapper']).resolve(strict=True)
    reconstructed = collect_sdk_closure(
        wrapper=wrapper, log=record['log']['path'], built=record['built']['path'],
        installed=record['installed']['path'], environment=record['environment'],
        exit_status=record['exitStatus'],
        native_receipt=record['nativeReceipt']['path'] if record.get('nativeReceipt') else None)
    if reconstructed != record:
        raise RuntimeError('SPEC201_SDK_LOG_DEPENDENCY_PROOF_MISMATCH')
    rows, links = record.get('rows'), record.get('links')
    if not rows or not links or len(links) != 1:
        raise RuntimeError('SPEC201_SDK_COMPILE_LINK_PROOF_MISSING')
    files = [record['log'], record['built'], record['installed']]
    for row in rows:
        if '-c' not in row['command'] or not row['dependencies']:
            raise RuntimeError('SPEC201_SDK_COMPILE_PROOF_INVALID')
        command = row['command']
        if (str((wrapper / command[command.index('-c') + 1]).resolve()) != row['source']['path']
                or str((wrapper / command[command.index('-o') + 1]).resolve()) != row['output']['path']
                or row['source'] not in row['dependencies']):
            raise RuntimeError('SPEC201_SDK_COMPILE_PROOF_INVALID')
        files += [row['compiler'], row['source'], row['dependencyFile'], row['output'],
                  *row['dependencies']]
    for link in links:
        if '-shared' not in link['command']:
            raise RuntimeError('SPEC201_SDK_LINK_PROOF_INVALID')
        command = link['command']
        objects = {str((wrapper / value).resolve()) for value in command if value.endswith('.o')}
        if (objects != {row['output']['path'] for row in rows}
                or str((wrapper / command[command.index('-o') + 1]).resolve()) != link['output']['path']):
            raise RuntimeError('SPEC201_SDK_LINK_PROOF_INVALID')
        files += [link['compiler'], link['output']]
    if record.get('nativeReceipt'):
        files.append(record['nativeReceipt'])
    for item in files:
        if identity(item['path']) != item:
            raise RuntimeError('SPEC201_SDK_CLOSURE_INPUT_CHANGED:' + item['path'])
    if (record['built']['sha256'] != record['installed']['sha256'] or
            links[0]['output']['sha256'] != record['built']['sha256']):
        raise RuntimeError('SPEC201_SDK_INSTALLED_BUILD_MISMATCH')
    built, installed = (binary_check(Path(record[key]['path'])) for key in ('built', 'installed'))
    if (built.get('status') != 'PASS' or installed.get('status') != 'PASS' or
            built['sharedLibraries'] != installed['sharedLibraries']):
        raise RuntimeError('SPEC201_SDK_RUNTIME_LIBRARY_MISMATCH')
    if wrapper.name != 'pythonWrapper' or not (wrapper / 'setup.py').is_file():
        raise RuntimeError('SPEC201_SDK_MAINTAINED_WRAPPER_REQUIRED')
    env = dict(os.environ, **record['environment'], PYTHONDONTWRITEBYTECODE='1')
    # Import the installed package with its source counterpart excluded.
    package = Path(record['installed']['path']).parent
    if package.name not in ('ndnsf', 'py_repoclient'):
        raise RuntimeError('SPEC201_SDK_PACKAGE_UNSUPPORTED')
    env['PYTHONPATH'] = str(package.parent)
    env['LD_LIBRARY_PATH'] = '/usr/local/lib'
    module = 'ndnsf._ndnsf' if package.name == 'ndnsf' else 'py_repoclient._py_repoclient'
    command = ['/usr/bin/python3', '-c',
               'import importlib; m=importlib.import_module(%r); '
               'assert m.__file__ == %r' % (module, record['installed']['path'])]
    subprocess.run(command, cwd='/', env=env, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if record.get('nativeReceipt'):
        root = wrapper.parent
        helper_env = dict(env, PYTHONPATH=str(wrapper))
        subprocess.run(['/usr/bin/python3', str(root / 'scripts/spec180_native_build.py'),
                        'verify', '--root', str(root), '--build-dir', str(root / 'build'),
                        '--manifest', record['nativeReceipt']['path']],
                       cwd=root, env=helper_env, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return record['built']
