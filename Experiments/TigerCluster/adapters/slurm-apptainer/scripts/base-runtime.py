#!/usr/bin/env python3
"""Prepare and verify the stable dependency base inside its container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import sysconfig
import zipfile

BASE = Path('/opt/ndn-base')
LEGACY = Path('/opt/ndnsf-di/current')
MANIFEST = BASE / 'manifest'
APP_LIBS = ('libndn-service-framework', 'libndnsf-distributed-inference',
            'libndn-svs', 'libndnsd', 'libnac-abe', 'libopenabe', 'librelic', 'librelic_ec')
APP_PACKAGES = ('ndnsf', 'py_repoclient', 'ndnsf_di', 'ndnsf_distributed_inference')
NUMPY_LIBS = {'libopenblas64_p-r0-0cf96a72.3.23.dev.so',
              'libgfortran-040039e1.so.5.0.0', 'libquadmath-96973f99.so.0.0.0'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def numpy_wheel_files(wheel, expected_digest):
    require(digest(wheel) == expected_digest, 'NUMPY_WHEEL_DIGEST')
    with zipfile.ZipFile(wheel) as archive:
        files = {row.filename: hashlib.sha256(archive.read(row)).hexdigest()
                 for row in archive.infolist()
                 if row.filename.startswith('numpy.libs/') and not row.is_dir()}
    require(set(files) == {'numpy.libs/' + name for name in NUMPY_LIBS},
            'NUMPY_PRIVATE_LIBRARY_SET')
    return files


def verify_numpy_files(site, files):
    actual = {str(p.relative_to(site)) for p in (site / 'numpy.libs').iterdir()}
    require(actual == set(files), 'NUMPY_INSTALLED_LIBRARY_SET')
    for relative, expected in files.items():
        path = site / relative
        require(path.is_file() and not path.is_symlink() and digest(path) == expected,
                'NUMPY_PRIVATE_LIBRARY_CHANGED:' + relative)


def remove(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def verify_link_origins(output):
    require('not found' not in output and 'undefined symbol:' not in output,
            'BASE_LINK_UNRESOLVED:' + output)
    for match in re.finditer(r'(?:=>\s+)?(/[^\s]+)', output):
        path = Path(match.group(1)).resolve(strict=True)
        require(any(path == root or root in path.parents for root in
                    (Path('/lib').resolve(), Path('/usr/lib').resolve(),
                     Path('/lib64').resolve(), Path('/usr/lib64').resolve(),
                     BASE / 'lib', Path('/opt/onnxruntime/lib'))),
                'BASE_LINK_ORIGIN:' + str(path))


def python_routes(site, expected=None):
    # These are the two ordinary startup hooks in the pinned parent. Reject
    # editable installs and additional executable/path injection hooks.
    allowed = {'coloredlogs.pth', 'distutils-precedence.pth'}
    hooks = list(site.glob('*.pth'))
    require(not list(site.glob('*.egg-link')), 'BASE_PYTHON_EDITABLE_INSTALL')
    require({p.name for p in hooks} <= allowed, 'BASE_PYTHON_UNKNOWN_PTH')
    result = {p.name: digest(p) for p in hooks}
    if expected is not None:
        require(result == expected, 'BASE_PYTHON_HOOK_CONTENT')
    return result


def ensure_toolchain():
    # The pinned parent is a runtime image, not a compiler image. Install only
    # inside the candidate rootfs; never borrow host compiler headers/libraries.
    env = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
    sources = Path('/etc/apt/sources.list')
    sources.write_text(sources.read_text().replace('http://archive.ubuntu.com',
                                                  'https://archive.ubuntu.com').replace(
                                                  'http://security.ubuntu.com',
                                                  'https://security.ubuntu.com'))
    subprocess.run(['/usr/bin/apt-get', '-o', 'Acquire::Retries=3', 'update'], env=env, check=True)
    subprocess.run(['/usr/bin/apt-get', 'install', '-y', '--no-install-recommends',
                    'g++', 'pkg-config', 'libboost-all-dev', 'libssl-dev'], env=env, check=True)
    for tool in ('g++', 'ld'):
        require(Path('/usr/bin', tool).is_file(), 'BASE_TOOLCHAIN_MISSING:' + tool)
    remove(Path('/var/lib/apt/lists'))


def prepare(lock_path, wheels):
    require(Path('/.singularity.d').is_dir() and os.geteuid() == 0,
            'BASE_CONTAINER_ROOT_REQUIRED')
    lock = json.loads(lock_path.read_text())
    require(list(sys.version_info[:2]) == lock['python'], 'BASE_PYTHON_ABI')
    require(platform.machine() == lock['architecture'], 'BASE_ARCHITECTURE')
    site = Path(sysconfig.get_path('purelib'))
    require(str(site) == '/opt/venv/lib/python3.10/site-packages', 'BASE_PYTHON_PREFIX')
    require((Path(sysconfig.get_path('include')) / 'Python.h').is_file(), 'BASE_PYTHON_HEADERS')
    wheel = wheels / lock['numpy']['filename']
    numpy_files = numpy_wheel_files(wheel, lock['numpy']['sha256'])
    ensure_toolchain()
    # pip uninstall cannot remove untracked leftovers from a damaged image.
    remove(site / 'numpy.libs')
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--no-index', '--no-deps',
                    '--force-reinstall', str(wheel)], check=True)
    verify_numpy_files(site, numpy_files)

    # Preserve the NDN SDK at the SIF+APP v2 path. The historical prefix is a
    # build compatibility view; no old framework/DI library may survive there.
    for name in ('lib/pkgconfig', 'bin', 'include', 'manifest'):
        (BASE / name).mkdir(parents=True, exist_ok=True)
    require((LEGACY / 'lib/libndn-cxx.so').is_file(), 'BASE_NDN_CXX_MISSING')
    ndn_libraries = list((LEGACY / 'lib').glob('libndn-cxx.so*'))
    for source in ndn_libraries:
        destination = BASE / 'lib' / source.name
        if source.is_symlink():
            target = source.resolve(strict=True).name
            require(target.startswith('libndn-cxx.so'), 'BASE_NDN_CXX_LINK_TARGET')
            destination.symlink_to(target)
        else:
            shutil.copy2(source, destination)
    for source in ndn_libraries:
        source.unlink()
        source.symlink_to(BASE / 'lib' / source.name)
    shutil.copytree(LEGACY / 'include/ndn-cxx', BASE / 'include/ndn-cxx', dirs_exist_ok=True)
    pc = (LEGACY / 'lib/pkgconfig/libndn-cxx.pc').read_text()
    (BASE / 'lib/pkgconfig/libndn-cxx.pc').write_text(pc.replace(str(LEGACY), str(BASE)))
    for source in (LEGACY / 'bin').iterdir():
        if source.name.startswith(('ndn', 'nfd')):
            shutil.copy2(source, BASE / 'bin' / source.name, follow_symlinks=False)
        else:
            remove(source)
    # OpenABE/RELIC remain available as explicitly selected build SDK inputs,
    # never in a loader search directory. APP must ship its own copies.
    sdk = BASE / 'sdk/lib'
    sdk.mkdir(parents=True, exist_ok=True)
    for prefix in (LEGACY, Path('/usr/local')):
        for pattern in ('libopenabe*', 'librelic*', 'libzsym*'):
            for path in (prefix / 'lib').glob(pattern):
                target = sdk / path.name
                if target.exists():
                    require(digest(path) == digest(target), 'BASE_SDK_DUPLICATE_ABI:' + path.name)
                else:
                    shutil.copy2(path, target)
                remove(path)
    for prefix in (LEGACY, Path('/usr/local'), BASE):
        for stem in APP_LIBS:
            for pattern in (stem + '.so*', stem + '.a'):
                for path in (prefix / 'lib').glob(pattern):
                    remove(path)
        for name in ('ndn-service-framework', 'ndnsf-di', 'ndn-svs', 'ndnsd', 'nac-abe'):
            remove(prefix / 'include' / name)
        for name in ('libndn-service-framework', 'ndnsf-distributed-inference',
                     'libndn-svs', 'ndnsd', 'libnac-abe'):
            remove(prefix / 'lib/pkgconfig' / (name + '.pc'))
    for package in APP_PACKAGES:
        remove(site / package)
        for pattern in (package + '-*.dist-info', package + '-*.egg-info'):
            for path in site.glob(pattern):
                remove(path)
        for path in site.glob(package + '.*'):
            remove(path)
    for prefix in (LEGACY, Path('/usr/local'), BASE):
        for pattern in ('di-native-*', 'App_ServiceController*', 'ndnsf-di-*'):
            for path in (prefix / 'bin').glob(pattern):
                remove(path)
    for path in ('/src/ndnsf', '/src/ndn-svs', '/src/nac-abe', '/src/ndn-sd',
                 '/opt/ndnsf-stage', '/opt/ndnsf-candidate', '/opt/ndnsf-app',
                 '/opt/ndnsf-di/replay', '/opt/ndnsf-di/current/manifest'):
        remove(Path(path))
    Path('/etc/ld.so.conf.d/ndnsf-stable-base.conf').write_text(
        '/opt/ndn-base/lib\n/opt/onnxruntime/lib\n')
    subprocess.run(['/sbin/ldconfig'], check=True)
    shutil.copy2(__file__, MANIFEST / 'base-runtime.py')
    shutil.copy2(lock_path, MANIFEST / 'base-runtime.lock.json')
    (MANIFEST / 'numpy-private-libraries.json').write_text(json.dumps(numpy_files, indent=2))
    (MANIFEST / 'python-routes.json').write_text(json.dumps(
        python_routes(site, lock['pythonStartupHooks']), indent=2))
    # This is an SDK/loader test, not a DI request-chain qualification.
    source = MANIFEST / 'base-smoke.cpp'
    source.write_text('#include <ndn-cxx/name.hpp>\n#include <onnxruntime_cxx_api.h>\n'
                      '#include <iostream>\nint main() {\n'
                      'ndn::Name n("/base/smoke"); Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"base");\n'
                      'if(n.size()!=2) return 1;\n'
                      'std::cout << n << " ORT " << OrtGetApiBase()->GetVersionString() << "\\n"; }\n')
    command = ['/usr/bin/g++', '-B/usr/bin', '-std=c++17', str(source),
                    '-I/opt/ndn-base/include', '-I/opt/onnxruntime/include',
                    '-L/opt/ndn-base/lib', '-L/opt/onnxruntime/lib',
                    '-Wl,-rpath,/opt/ndn-base/lib:/opt/onnxruntime/lib',
               '-lndn-cxx', '-lonnxruntime', '-o', str(MANIFEST / 'base-smoke')]
    (MANIFEST / 'compiler.json').write_text(json.dumps({
        'command': command, 'compiler': run('/usr/bin/g++', '--version'),
        'linker': run('/usr/bin/ld', '--version'),
        'packages': run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n')}, indent=2))
    subprocess.run(command, check=True)
    remove(Path('/build-input'))


def verify():
    site = Path(sysconfig.get_path('purelib'))
    lock = json.loads((MANIFEST / 'base-runtime.lock.json').read_text())
    require(list(sys.version_info[:2]) == lock['python'], 'BASE_PYTHON_ABI')
    verify_numpy_files(site, json.loads((MANIFEST / 'numpy-private-libraries.json').read_text()))
    require(python_routes(site, lock['pythonStartupHooks']) == json.loads((MANIFEST / 'python-routes.json').read_text()),
            'BASE_PYTHON_ROUTES_CHANGED')
    import numpy as np
    require(np.__version__ == lock['numpy']['version'], 'NUMPY_VERSION')
    a = np.array([[1., 2.], [3., 4.]])
    require(np.array_equal(a @ a, [[7., 10.], [15., 22.]]), 'NUMPY_BLAS_SMOKE')
    # Full dependency bases may contain the external NDN/crypto libraries;
    # Core/DI production objects always belong to the separate NDNSF layer.
    forbidden = APP_LIBS[:2] if (MANIFEST / 'dependency-sdk.json').is_file() else APP_LIBS
    for prefix in (LEGACY, Path('/usr/local'), BASE):
        for stem in forbidden:
            require(not list((prefix / 'lib').glob(stem + '.so*')), 'APPLICATION_LIBRARY_IN_BASE:' + stem)
    for name in APP_PACKAGES:
        require(not (site / name).exists() and not list(site.glob(name + '.*')),
                'APPLICATION_PACKAGE_IN_BASE:' + name)
    for prefix in (LEGACY, Path('/usr/local'), BASE):
        for pattern in ('di-native-*', 'App_ServiceController*', 'ndnsf-di-*'):
            require(not list((prefix / 'bin').glob(pattern)), 'APPLICATION_BINARY_IN_BASE:' + pattern)
    artifacts = [BASE / 'lib/libndn-cxx.so', Path('/opt/onnxruntime/lib/libonnxruntime.so'),
                 MANIFEST / 'base-smoke', BASE / 'bin/nfd', BASE / 'bin/nfdc']
    for path in artifacts:
        output = run('/usr/bin/ldd', '-r', str(path))
        verify_link_origins(output)
    result = {'status': 'PASS', 'scope': 'BASE_SMOKE_ONLY',
              'native': run(str(MANIFEST / 'base-smoke')).strip(),
              'nfd': run(str(BASE / 'bin/nfd'), '--version').strip(),
              'numpy': np.__version__, 'numpyPrivateLibraries': sorted(NUMPY_LIBS),
              'artifacts': {str(p): digest(p) for p in artifacts}}
    print(json.dumps(result, sort_keys=True))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'verify'))
    parser.add_argument('--lock', type=Path)
    parser.add_argument('--wheels', type=Path)
    args = parser.parse_args()
    if args.action == 'prepare':
        require(args.lock is not None and args.wheels is not None, 'BASE_INPUTS_REQUIRED')
        prepare(args.lock, args.wheels)
    else:
        verify()
