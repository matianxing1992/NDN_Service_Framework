#!/usr/bin/env python3
"""Read-only inventory; Waf remains authoritative for ABI/compile/link checks."""
import argparse
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

NIX_STORE = Path('/nix/store')
# These maintained DSOs currently have no SONAME; exact path/hash still bind them.
NIX_UNVERSIONED_DSOS = frozenset({
    'libopenabe.so', 'libndnsf-distributed-inference.so', 'libndnsf-uav-tracking.so'})


def _store_item(path):
    path = Path(path)
    return (path.is_absolute() and path.parent == NIX_STORE and
            re.fullmatch(r'[0-9a-z]{32}-.+', path.name) is not None and
            path.resolve() == path and path.exists())


def require_nix_path(identity, path):
    resolved = Path(path).resolve(strict=True)
    if not any(resolved == Path(root) or Path(root) in resolved.parents
               for root in identity['roots']):
        raise ValueError('undeclared Nix dependency path: ' + str(resolved))
    return str(resolved)


def _file_identity(path):
    return {'path': str(path), 'realpath': str(Path(path).resolve(strict=True)),
            'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}


def _elf_identity(readelf, path):
    text = subprocess.check_output([readelf, '-h', '-d', str(path)], text=True,
                                   env={**os.environ, 'LC_ALL': 'C'})
    def field(pattern):
        match = re.search(pattern, text)
        return match.group(1).strip() if match else ''
    return {'soname': field(r'Library soname: \[([^]]+)\]'),
            'elfClass': field(r'Class:\s*(\S+)'),
            'machine': field(r'Machine:\s*([^\n]+)'),
            'elfType': field(r'Type:\s*(\S+)'),
            'pie': bool(re.search(r'\(FLAGS_1\).*\bPIE\b', text))}


def make_nix_build_identity(spec):
    """Generate the immutable receipt inside a declared Nix derivation."""
    compiler = spec['compiler']
    prefix = spec['binutilsPrefix']
    tools = {'cxx': compiler, 'readelf': spec['readelf']}
    for name in ('ld', 'ar', 'ranlib', 'nm', 'cc1plus'):
        value = subprocess.check_output(
            [compiler, '-B' + prefix + '/', '-print-prog-name=' + name], text=True).strip()
        tools[name] = value if os.path.isabs(value) else shutil.which(value)
    receipt = {'schema': 'ndnsf-nix-build-identity-v1',
               'roots': sorted(set(spec['roots'])), 'boost': spec['boost'],
               'sdks': dict(spec.get('sdks', {})),
               'binutilsPrefix': prefix,
               'target': subprocess.check_output([compiler, '-dumpmachine'], text=True).strip(),
               'tools': {name: _file_identity(path) for name, path in tools.items()},
               'libraries': {}}
    for name, path in spec['libraries'].items():
        receipt['libraries'][name] = {**_file_identity(path), **_elf_identity(tools['readelf'], path)}
    validate_nix_build_identity(receipt)
    return receipt


def validate_nix_build_identity(identity):
    """Check exact roots, executable bytes and DSO ABI; never consult host receipts."""
    if not isinstance(identity, dict) or identity.get('schema') != 'ndnsf-nix-build-identity-v1':
        raise ValueError('invalid Nix build identity schema')
    roots = identity.get('roots')
    if (not isinstance(roots, list) or not 1 <= len(roots) <= 4096 or
            not all(isinstance(root, str) for root in roots) or
            len(set(roots)) != len(roots) or not all(_store_item(root) for root in roots)):
        raise ValueError('Nix identity requires exact immutable store outputs, not the whole store')
    if not identity.get('target', '').startswith('x86_64-'):
        raise ValueError('Nix compiler target is not the supported x86-64 ABI')
    prefix = require_nix_path(identity, identity['binutilsPrefix'])
    if not Path(prefix).is_dir():
        raise ValueError('Nix binutils prefix is not a directory')
    tools = identity['tools']
    for name in ('cxx', 'ld', 'ar', 'ranlib', 'nm', 'cc1plus', 'readelf'):
        entry = tools[name]
        actual = require_nix_path(identity, entry['path'])
        if (actual != entry['realpath'] or not os.access(actual, os.X_OK) or
                hashlib.sha256(Path(actual).read_bytes()).hexdigest() != entry['sha256']):
            raise ValueError('Nix tool identity changed: ' + name)
    for name, entry in identity['libraries'].items():
        actual = require_nix_path(identity, entry['path'])
        if (Path(entry['path']).name != name or actual != entry['realpath'] or
                hashlib.sha256(Path(actual).read_bytes()).hexdigest() != entry['sha256']):
            raise ValueError('Nix library digest/path changed: ' + name)
        abi = _elf_identity(tools['readelf']['realpath'], actual)
        if (abi != {key: entry[key] for key in
                    ('soname', 'elfClass', 'machine', 'elfType', 'pie')} or
                abi['elfClass'] != 'ELF64' or abi['machine'] != 'Advanced Micro Devices X86-64' or
                abi['elfType'] != 'DYN' or abi['pie'] or
                not abi['soname'] and name not in NIX_UNVERSIONED_DSOS):
            raise ValueError('Nix library ABI/SONAME changed: ' + name)
    boost = identity['boost']
    for key in ('includes', 'libraries'):
        require_nix_path(identity, boost[key])
    for prefix in identity['sdks'].values():
        require_nix_path(identity, prefix)
    version = (Path(boost['includes']) / 'boost/version.hpp').read_text()
    match = re.search(r'^#define BOOST_VERSION (\d+)$', version, re.MULTILINE)
    if not match or int(match.group(1)) != boost['version']:
        raise ValueError('Nix Boost header version changed')
    return identity


def validate_nix_compiler(identity, compiler):
    command = shlex.split(compiler) if isinstance(compiler, str) else list(compiler)
    flag = '-B' + identity['binutilsPrefix'] + '/'
    if not command or command[1:] not in ([], [flag]):
        raise ValueError('Nix compiler command contains undeclared arguments')
    compiler = str(Path(shutil.which(command[0]) or command[0]).resolve(strict=True))
    if compiler != identity['tools']['cxx']['realpath']:
        raise ValueError('Nix compiler differs from the declared toolchain')
    if subprocess.check_output([compiler, '-dumpmachine'], text=True).strip() != identity['target']:
        raise ValueError('Nix compiler target changed')
    for name in ('ld', 'ar', 'ranlib', 'nm', 'cc1plus'):
        path = subprocess.check_output(
            [compiler, '-B' + identity['binutilsPrefix'] + '/', '-print-prog-name=' + name],
            text=True).strip()
        path = require_nix_path(identity, shutil.which(path) or path)
        if path != identity['tools'][name]['realpath']:
            raise ValueError('Nix compiler resolved a different tool: ' + name)


def reject_nix_tool_overrides(arguments):
    for argument in arguments:
        if str(argument).startswith(('-B', '--prefix', '-specs', '--specs', '-fuse-ld',
                                     '-wrapper', '--sysroot', '-isysroot', '-fplugin')):
            raise ValueError('Nix toolchain override is not declared: ' + str(argument))


def validate_nix_library_selection(identity, directories, names):
    for name in names:
        key = 'lib' + name + '.so'
        if key not in identity['libraries']:
            continue
        selected = next((Path(directory) / key for directory in directories
                         if (Path(directory) / key).is_file()), None)
        if selected is None or str(selected.resolve()) != identity['libraries'][key]['realpath']:
            raise ValueError('Selected library differs from the Nix identity: ' + key)


@lru_cache(maxsize=1)
def load_nix_build_identity(path):
    # The cache key names an immutable store file, never a mutable experiment input.
    if not _store_item(path) or not Path(path).is_file() or Path(path).stat().st_size > 1024 * 1024:
        raise ValueError('Nix build identity must be an immutable store file')
    return validate_nix_build_identity(json.loads(Path(path).read_text()))


def nix_build_identity():
    path = os.environ.get('NDNSF_NIX_BUILD_IDENTITY', '')
    roots = os.environ.get('NDNSF_NIX_DEPENDENCY_ROOTS', '')
    if not path:
        if roots:
            raise ValueError('Nix dependency roots require a validated Nix build identity')
        return None
    if os.environ.get('NDNSF_CONTAINER_BUILD') == '1':
        raise ValueError('Nix build identity cannot use the container marker')
    identity = load_nix_build_identity(path)
    if roots and set(roots.split(os.pathsep)) != set(identity['roots']):
        raise ValueError('Nix dependency roots differ from the identity receipt')
    return identity


def resolve_components(value='all'):
    """Return the supported native component closure in dependency order."""
    profiles = {
        'core': ('core',),
        'repo': ('core', 'repo'),
        'di': ('core', 'repo', 'di'),
        'all': ('core', 'repo', 'di', 'uav'),
    }
    if value not in profiles:
        raise ValueError('components must be all, core, repo or di')
    return profiles[value]


def check_dependencies(nac_prefix='/usr/local', onnx_prefix='/usr/local', sdk_only=False,
                       components='all'):
    selected = resolve_components(components)
    needs_di = 'di' in selected
    needs_uav = 'uav' in selected
    if sdk_only and not needs_di:
        return [], []
    missing, optional = [], []
    for command in (() if sdk_only else ('g++', 'ar', 'readelf', 'protoc')):
        if not shutil.which(command):
            missing.append('tool: ' + command)
    pkg = shutil.which('pkgconf') or shutil.which('pkg-config')
    env = os.environ.copy()
    roots = [nac_prefix, onnx_prefix, '/usr/local', '/opt/onnxruntime']
    env['PKG_CONFIG_PATH'] = os.pathsep.join(
        [str(Path(root) / 'lib/pkgconfig') for root in roots] +
        [env.get('PKG_CONFIG_PATH', '')])
    required = ('libndn-cxx >= 0.8.0', 'libndn-svs >= 0.1.0',
                'libnac-abe', 'ndnsd', 'openssl', 'protobuf', 'sqlite3')
    if needs_di:
        required += ('onnxruntime >= 1.20.0',)
    if needs_uav:
        required += ('gtkmm-3.0',)
    if sdk_only:
        required = ('onnxruntime >= 1.20.0',) if needs_di else ()
    if not pkg:
        missing.append('tool: pkg-config/pkgconf (dependency metadata not checked)')
    else:
        for name in required:
            if subprocess.run([pkg, '--exists', name], env=env).returncode:
                missing.append('pkg-config: ' + name)
        gst = 'gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0'
        if needs_uav and not sdk_only and subprocess.run([pkg, '--exists', *gst.split()], env=env).returncode:
            optional.append('Optional GStreamer development packages missing; video backend disabled')
    files = [Path(nac_prefix) / 'include/nac-abe/consumer.hpp',
             Path(nac_prefix) / 'lib/libnac-abe.so']
    if sdk_only:
        files = []
    files += [Path(onnx_prefix) / suffix for suffix in (
        'include/onnx/checker.h', 'include/onnx/shape_inference/implementation.h',
        'lib/libonnx.a', 'lib/libonnx_proto.a')] if needs_di else []
    for path in files:
        if not path.is_file():
            missing.append('installed SDK file: ' + str(path))
    archives = [os.environ.get('NDNSF_TOKENIZER_BRIDGE_ARCHIVE', ''),
                '/usr/local/lib/libndnsf_tokenizer_bridge.a',
                '/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a']
    if needs_di and not any(path and Path(path).is_file() for path in archives):
        missing.append('installed SDK: libndnsf_tokenizer_bridge.a')
    return missing, optional


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nac-abe-prefix', default='/usr/local')
    parser.add_argument('--onnx-prefix', default='/usr/local')
    parser.add_argument('--components', choices=('all', 'core', 'repo', 'di'), default='all')
    parser.add_argument('--sdk-only', action='store_true',
                        help='Check preinstalled ONNX/ORT/tokenizer SDKs only')
    args = parser.parse_args()
    errors, warnings = check_dependencies(args.nac_abe_prefix, args.onnx_prefix, args.sdk_only,
                                          args.components)
    for message in warnings:
        print('OPTIONAL: ' + message)
    for message in errors:
        print('MISSING: ' + message)
    if errors and args.sdk_only:
        print('These SDKs are prerequisites, not installed by --source. '
              'See docs/unified-stack-install.md#explicit-limitations')
    if not errors:
        print('Inventory passed; Waf must still validate versions, ABI and link closure.')
    raise SystemExit(bool(errors))
