#!/usr/bin/env python3
"""Complete and verify external dependencies in an existing base container."""
import hashlib
import importlib.util
import json
import os
import re
import shlex
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

BASE = Path('/opt/ndn-base')
MANIFEST = BASE / 'manifest/dependency-sdk.json'
TOKENIZER_ARCHIVE = BASE / 'lib/libndnsf_tokenizer_bridge.a'
TOKENIZER_PROVENANCE = BASE / 'manifest/tokenizer-bridge'
NAC_PROVENANCE = BASE / 'manifest/nac-abe'
NAC_SUITES = ('TestProducerBoundCk', 'TestConsumerRevocation', 'TestConsumer', 'TestProducer')
TOKENIZER_SYMBOLS = frozenset('ndi_token_create ndi_token_encode ndi_token_decode '
                              'ndi_token_decode_stable ndi_token_free ndi_token_destroy'.split())


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def run(*args, **kwargs):
    return subprocess.check_output(list(args), text=True, stderr=subprocess.STDOUT, **kwargs)


def check(*args, **kwargs):
    subprocess.run(list(args), check=True, **kwargs)


def nac_contract(inputs, repository, archive_sha):
    """Validate the sealed host lock/patch stack against the original SDK source."""
    inputs = Path(inputs)
    frozen = json.loads((inputs / 'inputs.json').read_text())
    required = {'upstream.tar', 'project/scripts/stack_sources.py',
                'project/packaging/host-dependencies.lock.json',
                'project/patches/nac-abe/series.json', 'project/patches/openabe/series.json'}
    if (not required.issubset(frozen['files']) or any(
            name not in required and not (
                name.startswith(('project/patches/nac-abe/', 'project/patches/openabe/')) and
                name.endswith('.patch'))
            for name in frozen['files'])):
        raise ValueError('SDK_NAC_INPUT_SET')
    for name, expected in frozen['files'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts or str(relative) != name:
            raise ValueError('SDK_NAC_INPUT_PATH')
        path = inputs / relative
        if (any(p.is_symlink() for p in (path, *path.parents)) or
                not path.is_file() or digest(path) != expected):
            raise ValueError('SDK_NAC_INPUT_CHANGED:' + name)
    if frozen['files'].get('upstream.tar') != archive_sha:
        raise ValueError('SDK_NAC_ARCHIVE_BINDING')
    project = inputs / 'project'
    module = importlib.util.spec_from_file_location('nac_stack_sources',
                                                   project / 'scripts/stack_sources.py')
    helper = importlib.util.module_from_spec(module)
    exec(compile(Path(module.origin).read_bytes(), module.origin, 'exec'), helper.__dict__)
    # Preserve the existing full lock validation, including OpenABE patch
    # metadata. Only the NAC source is rebuilt by this completion.
    lock = helper.load_lock(project / 'packaging/host-dependencies.lock.json')
    source = lock['sources']['NAC-ABE']
    if (source['commit'] != repository['revision'] or
            source['url'] != repository.get('url', source['url'])):
        raise ValueError('SDK_NAC_UPSTREAM_BINDING')
    patches = helper.patch_series(source, project)
    series = source['patch_series']['path']
    names = {'upstream.tar', 'project/scripts/stack_sources.py',
             'project/packaging/host-dependencies.lock.json', 'project/' + series}
    names.update('project/' + str(Path(series).parent / name) for name, _ in patches)
    openabe = lock['sources']['openabe']
    names.add('project/' + openabe['patch_series']['path'])
    names.update('project/' + str(Path(openabe['patch_series']['path']).parent / name)
                 for name, _ in helper.patch_series(openabe, project))
    if set(frozen['files']) != names:
        raise ValueError('SDK_NAC_INPUT_SET')
    return frozen, helper, source, patches


def nac_owned(name):
    relative = Path(name)
    return (not relative.is_absolute() and '..' not in relative.parts and
            (str(relative).startswith('include/nac-abe/') or
             str(relative) in ('lib/libnac-abe.so', 'lib/pkgconfig/libnac-abe.pc')))


def nac_native_tests(executable, prefix, schema):
    """Exercise existing native crypto/admission cases against the selected library."""
    prefix = Path(prefix)
    env = dict(os.environ)
    env.pop('LD_PRELOAD', None)
    env['LD_LIBRARY_PATH'] = str(prefix / 'lib') + ':' + str(BASE / 'lib')
    report = run('/usr/bin/ldd', '-r', str(executable), env=env)
    selected = re.search(r'libnac-abe\.so\s+=>\s+(\S+)', report)
    if ('not found' in report or 'undefined symbol:' in report or not selected or
            Path(selected.group(1)).resolve() != (prefix / 'lib/libnac-abe.so').resolve()):
        raise ValueError('SDK_NAC_TEST_LIBRARY_ORIGIN')
    with tempfile.TemporaryDirectory(prefix='nac-native-tests-') as temporary:
        shutil.copy2(schema, Path(temporary) / 'trust-schema.conf')
        env['HOME'] = temporary
        for suite in NAC_SUITES:
            check(str(executable), '--run_test=' + suite, '--report_level=detailed',
                  '--log_level=test_suite', '--color_output=no', cwd=temporary, env=env,
                  timeout=180)


def patched_nac_source(inputs, work, patches):
    """Apply admitted patches in private scratch using the existing container tool."""
    work = Path(work)
    source_root = work / 'source'
    source_root.mkdir()
    with tarfile.open(Path(inputs) / 'upstream.tar') as archive:
        seen = set()
        for member in archive:
            name = Path(member.name)
            if (name.is_absolute() or '..' in name.parts or member.name in seen or
                    not (member.isfile() or member.isdir())):
                raise ValueError('SDK_NAC_SOURCE_MEMBER')
            seen.add(member.name)
        archive.extractall(source_root)
    patch_root = work / 'patches'
    patch_root.mkdir()
    for name, content in patches:
        patch = patch_root / name
        patch.write_bytes(content)
        for dry_run in (True, False):
            arguments = ['--batch', '--forward', '--fuzz=0', '--no-backup-if-mismatch',
                         '-p1', '--input=' + str(patch)]
            if dry_run:
                arguments.append('--dry-run')
            check('/usr/bin/patch', *arguments, cwd=source_root)
    return source_root


def build_nac(inputs, work, repository, archive_sha):
    """Build the existing patch stack once, with a private staged install."""
    frozen, helper, source, patches = nac_contract(inputs, repository, archive_sha)
    work = Path(work)
    source_root = patched_nac_source(inputs, work, patches)
    build = work / 'build'
    check('/usr/bin/cmake', '-S', str(source_root), '-B', str(build),
          '-DCMAKE_BUILD_TYPE=Release', '-DHAVE_TESTS=ON', '-DBUILD_EXAMPLES=OFF',
          '-DCMAKE_CXX_COMPILER=/usr/bin/g++',
          '-DCMAKE_CXX_FLAGS=-B/usr/bin -DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES',
          '-DCMAKE_INSTALL_PREFIX=' + str(BASE), '-DCMAKE_INSTALL_LIBDIR=lib',
          '-DCMAKE_PREFIX_PATH=' + str(BASE), '-DCMAKE_LIBRARY_PATH=' + str(BASE / 'lib'),
          '-DCMAKE_INSTALL_RPATH=$ORIGIN', '-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON',
          '-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF')
    check('/usr/bin/cmake', '--build', str(build), '--parallel', '2')
    stage = work / 'stage'
    check('/usr/bin/cmake', '--install', str(build), '--prefix', str(stage))
    names = set()
    for entry in (build / 'install_manifest.txt').read_text().splitlines():
        path = Path(entry)
        name = str(path.relative_to(stage))
        if (not nac_owned(name) or path.is_symlink() or not path.is_file() or
                path.resolve() != path):
            raise ValueError('SDK_NAC_INSTALL_OWNERSHIP:' + name)
        names.add(name)
    observed = {str(path.relative_to(stage)) for path in stage.rglob('*') if path.is_file()}
    if names != observed or 'lib/libnac-abe.so' not in names:
        raise ValueError('SDK_NAC_INSTALL_MANIFEST')
    executable = build / 'tests/unit-tests'
    schema = source_root / 'tests/unit-tests/trust-schema.conf'
    nac_native_tests(executable, stage, schema)
    return stage, names, executable, schema, frozen, source


def install_nac(stage, names, executable, schema, inputs, frozen, source):
    """Install only staged manifest outputs and their explicit verification evidence."""
    for name in sorted(names):
        destination = BASE / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(stage / name, destination)
    shutil.copytree(inputs, NAC_PROVENANCE)
    shutil.copy2(executable, NAC_PROVENANCE / 'unit-tests')
    shutil.copy2(schema, NAC_PROVENANCE / 'trust-schema.conf')
    # The base declares this library directory before the SDK populates it.
    # Refresh its cache so indirect Relic dependencies also resolve without
    # LD_LIBRARY_PATH; retain the strict installed API/runtime probe below.
    check('/sbin/ldconfig', '-X')
    helper_path = NAC_PROVENANCE / 'project/scripts/stack_sources.py'
    module = importlib.util.spec_from_file_location('installed_nac_stack', helper_path)
    helper = importlib.util.module_from_spec(module)
    exec(compile(helper_path.read_bytes(), str(helper_path), 'exec'), helper.__dict__)
    helper.probe_nac_api(BASE)
    nac_native_tests(NAC_PROVENANCE / 'unit-tests', BASE, NAC_PROVENANCE / 'trust-schema.conf')
    return {'upstreamCommit': source['commit'],
            'patchSeriesSha256': source['patch_series']['sha256'],
            'hostDependencyLockSha256': frozen['files']['project/packaging/host-dependencies.lock.json'],
            'sourceFiles': {name[len('project/'):]: 'sha256:' + sha
                            for name, sha in frozen['files'].items() if name.startswith('project/')},
            'installedFiles': {str(BASE / name): digest(BASE / name) for name in sorted(names)},
            'abi': 'VERIFIED', 'nativeTests': 'PASS', 'nativeSuites': list(NAC_SUITES)}


def complete_nac(inputs=Path('/build-input/nac')):
    """Complete the existing SDK using exactly the already-locked NAC patch stack."""
    if os.geteuid() != 0 or not Path('/.singularity.d').is_dir():
        raise ValueError('SDK_CONTAINER_REQUIRED')
    original = MANIFEST.read_bytes()
    body = json.loads(original)
    frozen = json.loads((Path(inputs) / 'inputs.json').read_text())
    if frozen['baseSdkSha256'] != 'sha256:' + hashlib.sha256(original).hexdigest():
        raise ValueError('SDK_NAC_PARENT_MANIFEST')
    if NAC_PROVENANCE.exists() or 'nacAbePatchClosure' in body:
        raise ValueError('SDK_NAC_OUTPUT_EXISTS')
    verify()
    with tempfile.TemporaryDirectory(prefix='nac-sdk-build-') as temporary:
        stage, names, executable, schema, frozen, source = build_nac(
            inputs, temporary, body['inputs']['repositories']['nacAbe'],
            body['inputs']['files']['source/nacAbe.tar'])
        for name, expected in body['artifacts'].items():
            if digest(name) != expected:
                raise ValueError('SDK_NAC_UNRELATED_BUILD_DRIFT:' + name)
            relative = Path(name).relative_to(BASE) if BASE in Path(name).parents else None
            if relative and nac_owned(str(relative)) and str(relative) not in names:
                raise ValueError('SDK_NAC_OUTPUT_REMOVAL_UNSUPPORTED:' + name)
        if (MANIFEST.read_bytes() != original or
                run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n') != body['packages']):
            raise ValueError('SDK_NAC_PARENT_CHANGED')
        closure = install_nac(stage, names, executable, schema, inputs, frozen, source)
    for name, expected in body['artifacts'].items():
        if name not in closure['installedFiles'] and digest(name) != expected:
            raise ValueError('SDK_NAC_UNRELATED_INSTALL_DRIFT:' + name)
    if run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n') != body['packages']:
        raise ValueError('SDK_NAC_PACKAGES_CHANGED')
    artifacts = dict(body['artifacts'])
    artifacts.update(closure['installedFiles'])
    for path in NAC_PROVENANCE.rglob('*'):
        if path.is_file():
            artifacts[str(path)] = digest(path)
    shutil.copy2(__file__, BASE / 'manifest/dependency-sdk.py')
    artifacts[str(BASE / 'manifest/dependency-sdk.py')] = digest(BASE / 'manifest/dependency-sdk.py')
    body.update(artifacts=artifacts, nacAbePatchClosure=closure)
    MANIFEST.write_text(json.dumps(body, indent=2) + '\n')
    verify()
    (BASE / 'manifest/nac-abe-completion.json').write_text(json.dumps({
        'status': 'PASS', 'parentSdkSha256': hashlib.sha256(original).hexdigest(),
        'nacOwnedOutputs': sorted(closure['installedFiles']),
        'nativeTests': 'PASS', 'nativeSuites': list(NAC_SUITES)}, indent=2) + '\n')


def verify_tokenizer(archive, provenance):
    """Link production ABI callers against this exact archive and frozen vectors."""
    with Path(archive).open('rb') as stream:
        if stream.read(8) != b'!<arch>\n':
            raise ValueError('SDK_TOKENIZER_ARCHIVE_FORMAT')
    symbols = {line.split()[-1] for line in run(
        '/usr/bin/nm', '-g', '--defined-only', str(archive)).splitlines() if line.split()}
    if not TOKENIZER_SYMBOLS.issubset(symbols):
        raise ValueError('SDK_TOKENIZER_SYMBOLS')
    provenance = Path(provenance)
    with tempfile.TemporaryDirectory(prefix='tokenizer-probe-') as temporary:
        for source, vector in [('probe.cpp', 'vectors.json'),
                               ('stable-probe.cpp', 'stable-vectors.json')]:
            executable = Path(temporary) / Path(source).stem
            check('/usr/bin/g++', '-B/usr/bin', '-std=c++17',
                  str(provenance / 'probes' / source), '-I' + str(provenance / 'crate'),
                  str(archive), '-ldl', '-lpthread', '-lm', '-lrt', '-lutil',
                  '-o', str(executable))
            check(str(executable), str(provenance / 'probes' / vector))


def complete_tokenizer(inputs=Path('/build-input/tokenizer')):
    """Extend a verified SDK with its missing offline tokenizer static library only."""
    if os.geteuid() != 0 or not Path('/.singularity.d').is_dir():
        raise ValueError('SDK_CONTAINER_REQUIRED')
    original = MANIFEST.read_bytes()
    body = json.loads(original)
    inputs = Path(inputs)
    frozen = json.loads((inputs / 'inputs.json').read_text())
    if 'sha256:' + hashlib.sha256(original).hexdigest() != frozen['baseSdkSha256']:
        raise ValueError('SDK_TOKENIZER_PARENT_MANIFEST')
    names = {'crate/Cargo.toml', 'crate/Cargo.lock', 'crate/src/lib.rs',
             'crate/tokenizer-abi.h', 'native-inputs.json', 'probes/probe.cpp',
             'probes/stable-probe.cpp', 'probes/vectors.json', 'probes/stable-vectors.json'}
    if set(frozen['files']) != names:
        raise ValueError('SDK_TOKENIZER_INPUT_SET')
    for name, expected in frozen['files'].items():
        path = inputs / name
        if path.is_symlink() or not path.is_file() or digest(path) != expected:
            raise ValueError('SDK_TOKENIZER_INPUT_CHANGED:' + name)
    native = json.loads((inputs / 'native-inputs.json').read_text())
    if (body['inputs']['files']['native/native-inputs.json'] !=
            frozen['files']['native-inputs.json'] or native['rustVersion'] != '1.90.0'):
        raise ValueError('SDK_TOKENIZER_NATIVE_BINDING')
    for name in ('Cargo.toml', 'Cargo.lock'):
        if native['crate'][name] != 'sha256:' + frozen['files']['crate/' + name]:
            raise ValueError('SDK_TOKENIZER_CRATE_BINDING:' + name)
    if TOKENIZER_ARCHIVE.exists() or TOKENIZER_ARCHIVE.is_symlink() or TOKENIZER_PROVENANCE.exists():
        raise ValueError('SDK_TOKENIZER_OUTPUT_EXISTS')
    # Validate the existing SDK before compiling anything. Its historical
    # manifest does not claim that a tokenizer archive exists.
    verify()
    rustc = run('/opt/rust/bin/rustc', '--version').strip()
    cargo = run('/opt/rust/bin/cargo', '--version').strip()
    if rustc.split()[1] != '1.90.0' or cargo.split()[1] != '1.90.0':
        raise ValueError('SDK_TOKENIZER_TOOLCHAIN')
    with tempfile.TemporaryDirectory(prefix='tokenizer-build-') as temporary:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('CARGO_', 'RUST', 'CC_', 'CXX_', 'LD_'))}
        env.update(PATH='/opt/rust/bin:/usr/bin:/bin:/usr/sbin:/sbin',
                   CARGO_HOME='/opt/cargo-home', RUSTC='/opt/rust/bin/rustc',
                   CARGO_NET_OFFLINE='true', CC='/usr/bin/gcc', CXX='/usr/bin/g++',
                   CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER='/usr/bin/gcc')
        check('/opt/rust/bin/cargo', 'build', '--offline', '--locked', '--release',
              '--jobs', '2', '--manifest-path', str(inputs / 'crate/Cargo.toml'),
              '--target-dir', temporary, env=env)
        archive = Path(temporary) / 'release/libndnsf_tokenizer_bridge.a'
        verify_tokenizer(archive, inputs)
        # Recheck all declared sources and original SDK artifacts before
        # installing new outputs. No pre-existing SDK artifact is replaced.
        for name, expected in frozen['files'].items():
            if digest(inputs / name) != expected:
                raise ValueError('SDK_TOKENIZER_INPUT_CHANGED:' + name)
        for name, expected in body['artifacts'].items():
            if digest(name) != expected:
                raise ValueError('SDK_ARTIFACT_CHANGED:' + name)
        if (MANIFEST.read_bytes() != original or
                run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n') != body['packages']):
            raise ValueError('SDK_TOKENIZER_PARENT_CHANGED')
        added = [TOKENIZER_ARCHIVE, BASE / 'manifest/dependency-sdk.py',
                 BASE / 'manifest/tokenizer-bridge-completion.json']
        added += [TOKENIZER_PROVENANCE / name for name in sorted(names)]
        if any(str(p) in body['artifacts'] for p in added):
            raise ValueError('SDK_TOKENIZER_ARTIFACT_CONFLICT')
        TOKENIZER_ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(archive, TOKENIZER_ARCHIVE)
    for name in sorted(names):
        target = TOKENIZER_PROVENANCE / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(inputs / name, target)
    shutil.copy2(__file__, BASE / 'manifest/dependency-sdk.py')
    receipt = {'status': 'VERIFIED', 'scope': 'TOKENIZER_BRIDGE_COMPLETION',
               'parentSdkSha256': hashlib.sha256(original).hexdigest(),
               'sourceSealDigest': frozen['sourceSealDigest'], 'inputs': frozen['files'],
               'archiveSha256': digest(TOKENIZER_ARCHIVE), 'rustc': rustc, 'cargo': cargo,
               'abiSymbols': sorted(TOKENIZER_SYMBOLS), 'nativeProbes': 'PASS'}
    (BASE / 'manifest/tokenizer-bridge-completion.json').write_text(
        json.dumps(receipt, indent=2) + '\n')
    body['tokenizerBridge'] = receipt
    body['artifacts'].update({str(p): digest(p) for p in added})
    MANIFEST.write_text(json.dumps(body, indent=2) + '\n')
    verify()


def prepare():
    assert os.geteuid() == 0 and Path('/.singularity.d').is_dir(), 'SDK_CONTAINER_REQUIRED'
    inputs = Path('/build-input/sdk')
    expected = json.loads((inputs / 'inputs.json').read_text())
    for name, value in expected['files'].items():
        assert digest(inputs / name) == value, 'SDK_INPUT_CHANGED:' + name
    env = os.environ
    env.update(PATH='/usr/bin:/bin:/usr/sbin:/sbin:/opt/ndn-base/bin:/opt/venv/bin',
               DEBIAN_FRONTEND='noninteractive', CC='/usr/bin/gcc', CXX='/usr/bin/g++',
               CXXFLAGS='-B/usr/bin -DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES',
               LDFLAGS='-B/usr/bin', LD_LIBRARY_PATH='/opt/ndn-base/lib:/opt/onnxruntime/lib',
               PKG_CONFIG_PATH='/opt/ndn-base/lib/pkgconfig:/opt/onnxruntime/lib/pkgconfig',
               CPLUS_INCLUDE_PATH='/opt/ndn-base/include', LIBRARY_PATH='/opt/ndn-base/lib')
    check('/opt/venv/bin/python', '-m', 'pip', 'install', '--no-index', '--no-deps',
          '--force-reinstall', *[str(p) for p in sorted((inputs / 'wheels').glob('*.whl'))])
    check('/usr/bin/apt-get', '-o', 'Acquire::Retries=3', 'update')
    check('/usr/bin/apt-get', 'install', '-y', '--no-install-recommends',
          'build-essential', 'cmake', 'pkg-config', 'libgmp-dev', 'libssl-dev',
          'libsqlite3-dev', 'libboost-all-dev', 'libpcap-dev', 'protobuf-compiler',
          'libprotobuf-dev', 'libgtkmm-3.0-dev', 'ca-certificates',
          'libgstreamer1.0-dev', 'libgstreamer-plugins-base1.0-dev',
          'gstreamer1.0-tools', 'gstreamer1.0-plugins-base', 'libffi-dev', 'libpcre3-dev')
    for name in ['openabe', 'relic', 'relic_ec']:
        shutil.copytree(Path('/opt/ndnsf-di/current/include') / name,
                        BASE / 'include' / name, dirs_exist_ok=True)
    for path in (BASE / 'sdk/lib').iterdir():
        if path.is_file():
            shutil.copy2(path, BASE / 'lib' / path.name)
    sources = BASE / 'sdk/sources'
    sources.mkdir(parents=True, exist_ok=True)
    for name, archive in [('onnx', 'native/onnx.tar'),
                          ('ndn-svs', 'source/ndn-svs.tar'), ('ndn-sd', 'source/ndnSd.tar')]:
        dest = sources / name
        dest.mkdir()
        check('/bin/tar', '-xf', str(inputs / archive), '-C', str(dest))
    Path('/opt/cargo-vendor').mkdir()
    check('/bin/tar', '-xf', str(inputs / 'native/vendor.tar'), '-C', '/opt/cargo-vendor')
    for name in ['cargo', 'rustc', 'rust-std']:
        installer = Path('/tmp/sdk-rust-install')
        installer.mkdir()
        archive = inputs / 'native' / (name + '-1.90.0-x86_64-unknown-linux-gnu.tar.xz')
        check('/bin/tar', '-xf', str(archive), '-C', str(installer))
        check(str(next(installer.iterdir()) / 'install.sh'), '--prefix=/opt/rust', '--disable-ldconfig')
        shutil.rmtree(installer)
    Path('/opt/cargo-home').mkdir()
    Path('/opt/cargo-home/config.toml').write_text(
        '[source.crates-io]\nreplace-with="vendored-sources"\n'
        '[source.vendored-sources]\ndirectory="/opt/cargo-vendor"\n')
    check('/usr/bin/cmake', '-S', str(sources / 'onnx'), '-B', '/tmp/sdk-onnx-build',
          '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=/usr/bin/g++',
          '-DCMAKE_CXX_FLAGS=-B/usr/bin', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
          '-DCMAKE_INSTALL_PREFIX=/opt/onnx', '-DBUILD_ONNX_PYTHON=OFF',
          '-DONNX_USE_LITE_PROTO=OFF', '-DONNX_BUILD_TESTS=OFF', '-DONNX_ML=ON',
          '-DONNX_GEN_PB_TYPE_STUBS=OFF')
    check('/usr/bin/cmake', '--build', '/tmp/sdk-onnx-build', '--parallel', '4')
    check('/usr/bin/cmake', '--install', '/tmp/sdk-onnx-build')
    with tempfile.TemporaryDirectory(prefix='nac-sdk-build-') as temporary:
        stage, names, executable, schema, frozen, source = build_nac(
            inputs / 'nac', temporary, expected['repositories']['nacAbe'],
            expected['files']['source/nacAbe.tar'])
        nac_info = install_nac(stage, names, executable, schema, inputs / 'nac', frozen, source)
    env.update(CXX='/usr/bin/g++ -B/usr/bin', CPPFLAGS='-I/opt/ndn-base/include',
               LINKFLAGS='-L/opt/ndn-base/lib -Wl,-rpath,$ORIGIN',
               LDFLAGS='-B/usr/bin -L/opt/ndn-base/lib -Wl,-rpath,$ORIGIN')
    for name in ['ndn-svs', 'ndn-sd']:
        cwd = sources / name
        check('./waf', 'configure', '--prefix=/opt/ndn-base', '--libdir=/opt/ndn-base/lib',
              '--boost-includes=/usr/include', '--boost-libs=/usr/lib/x86_64-linux-gnu', cwd=cwd)
        check('./waf', '-j4', '-v', cwd=cwd)
        check('./waf', 'install', '-j4', cwd=cwd)
    for package in ['libnac-abe', 'libndn-svs', 'ndnsd']:
        assert run('pkg-config', '--variable=includedir', package).strip() == '/opt/ndn-base/include'
        assert run('pkg-config', '--variable=libdir', package).strip() == '/opt/ndn-base/lib'
    # Preserve the exact SVS source/build pair for NDNSF's ABI preflight.
    for name in ['onnx', 'ndn-sd']:
        shutil.rmtree(sources / name)
    for path in ['/tmp/sdk-onnx-build', '/var/lib/apt/lists']:
        shutil.rmtree(path)
    shutil.copy2(__file__, BASE / 'manifest/dependency-sdk.py')
    artifacts = {}
    for directory in [BASE / 'lib', BASE / 'include', BASE / 'sdk/sources', NAC_PROVENANCE, Path('/opt/onnx'),
                      Path('/opt/rust'), Path('/opt/cargo-vendor'), Path('/opt/cargo-home')]:
        for path in directory.rglob('*'):
            if path.is_file():
                artifacts[str(path)] = digest(path)
    MANIFEST.write_text(json.dumps({'schema': 'ndnsf-base-sdk-v1', 'inputs': expected,
        'artifacts': artifacts, 'packages': run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n'),
        'status': 'BUILT_NOT_TESTED', 'nacAbePatchClosure': nac_info}, indent=2) + '\n')
    verify()


def verify():
    body = json.loads(MANIFEST.read_text())
    assert body['schema'] == 'ndnsf-base-sdk-v1'
    assert run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n') == body['packages'], 'SDK_PACKAGES_CHANGED'
    for name, value in body['artifacts'].items():
        assert digest(name) == value, 'SDK_ARTIFACT_CHANGED:' + name
    if 'tokenizerBridge' in body:
        assert digest(TOKENIZER_ARCHIVE) == body['tokenizerBridge']['archiveSha256'], 'SDK_TOKENIZER_CHANGED'
        verify_tokenizer(TOKENIZER_ARCHIVE, TOKENIZER_PROVENANCE)
    if 'nacAbePatchClosure' in body:
        info = body['nacAbePatchClosure']
        frozen, helper, source, _ = nac_contract(
            NAC_PROVENANCE, body['inputs']['repositories']['nacAbe'],
            body['inputs']['files']['source/nacAbe.tar'])
        if (info['abi'] != 'VERIFIED' or info['nativeTests'] != 'PASS' or
                info['nativeSuites'] != list(NAC_SUITES) or
                info['upstreamCommit'] != source['commit'] or
                info['patchSeriesSha256'] != source['patch_series']['sha256'] or
                info['hostDependencyLockSha256'] !=
                frozen['files']['project/packaging/host-dependencies.lock.json'] or
                info['sourceFiles'] != {name[len('project/'):]: 'sha256:' + sha
                                        for name, sha in frozen['files'].items()
                                        if name.startswith('project/')}):
            raise ValueError('SDK_NAC_RECEIPT')
        if (not info['installedFiles'] or str(BASE / 'lib/libnac-abe.so') not in info['installedFiles'] or
                any(not nac_owned(str(Path(name).relative_to(BASE))) or
                    body['artifacts'].get(name) != sha
                    for name, sha in info['installedFiles'].items())):
            raise ValueError('SDK_NAC_INSTALLED_SET')
        for name, expected in info['installedFiles'].items():
            if digest(name) != expected:
                raise ValueError('SDK_NAC_INSTALLED_ARTIFACT_CHANGED:' + name)
        helper.probe_nac_api(BASE)
        nac_native_tests(NAC_PROVENANCE / 'unit-tests', BASE, NAC_PROVENANCE / 'trust-schema.conf')
    for name in ['libnac-abe.so', 'libndn-svs.so.0.1.0', 'libndnsd.so.0.1.0',
                 'libopenabe.so', 'librelic.so', 'librelic_ec.so']:
        result = run('/usr/bin/ldd', '-r', str(BASE / 'lib' / name))
        assert 'not found' not in result and 'undefined symbol:' not in result, 'SDK_LINK_CLOSURE:' + name
        allowed = [BASE / 'lib', Path('/opt/onnxruntime/lib'), Path('/lib'),
                   Path('/lib64'), Path('/usr/lib')]
        for value in re.findall(r'(?:=>\s+)?(/[^\s]+)', result):
            actual = Path(value).resolve()
            assert any(root.resolve() == actual or root.resolve() in actual.parents
                       for root in allowed), 'SDK_WRONG_ORIGIN:' + value
    assert '1.90.0' in run('/opt/rust/bin/rustc', '--version')
    assert '1.90.0' in run('/opt/rust/bin/cargo', '--version')
    # Real C++ full-protobuf ONNX consumer; no NDNSF objects enter base.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='sdk-probe-') as tmp:
        root = Path(tmp)
        source = root / 'probe.cpp'
        source.write_text('#include <onnx/onnx_pb.h>\n#include <onnx/checker.h>\n'
                          '#include <onnx/shape_inference/implementation.h>\n#include <ndn-cxx/name.hpp>\n'
                          '#include <nac-abe/consumer.hpp>\n#include <ndn-svs/svspubsub.hpp>\n'
                          '#include <ndnsd/discovery/service-discovery.hpp>\n'
                          '#include <gst/gst.h>\n#include <gtk/gtk.h>\n'
                          '#include <dlfcn.h>\nint main(){onnx::ModelProto p; p.set_ir_version(10);'
                          'p.add_opset_import()->set_version(13);auto g=p.mutable_graph();g->set_name("sdk");'
                          'auto x=g->add_input();x->set_name("x");auto t=x->mutable_type()->mutable_tensor_type();'
                          't->set_elem_type(onnx::TensorProto::FLOAT);t->mutable_shape()->add_dim()->set_dim_value(1);'
                          'auto y=g->add_output();*y=*x;y->set_name("y");auto op=g->add_node();'
                          'op->set_op_type("Identity");op->add_input("x");op->add_output("y");'
                          'onnx::shape_inference::InferShapes(p);onnx::checker::check_model(p);'
                          'std::string s; if(!p.SerializeToString(&s)||s.empty())return 1;'
                          'ndn::Name n("/sdk/probe");if(n.size()!=2)return 2;'
                          'auto interest=ndn::nacabe::ParamFetcher::getDefaultInterestTemplate();'
                          'volatile auto nac=&ndn::nacabe::Consumer::obtainDecryptionKey;'
                          'volatile auto svs=&ndn::svs::SVSPubSub::subscribeToProducerWithCatchUp;'
                          'volatile auto sd=&ndnsd::discovery::ServiceDiscovery::publishServiceDetail;'
                          'if(nac==nullptr||svs==nullptr||sd==nullptr)return 4;'
                          'gst_init(nullptr,nullptr);if(gtk_get_major_version()<3)return 5;'
                          'for(auto lib:{"libnac-abe.so","libndn-svs.so","libndnsd.so"})'
                          '{auto h=dlopen(lib,RTLD_NOW);if(!h)return 3;dlclose(h);}return 0;}\n')
        flags = shlex.split(run('/usr/bin/pkg-config', '--cflags', '--libs', 'libnac-abe',
                                'libndn-svs', 'ndnsd', 'gtkmm-3.0', 'gstreamer-1.0',
                                'gstreamer-app-1.0', 'gstreamer-video-1.0'))
        check('/usr/bin/g++', '-B/usr/bin', '-std=c++17', '-DONNX_ML', str(source),
              '-DONNX_NAMESPACE=onnx',
              '-DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES',
              '-I/opt/onnx/include', '-I/opt/ndn-base/include',
              '-I/opt/ndn-base/include/nac-abe', '-L/opt/onnx/lib',
              '-L/opt/ndn-base/lib', '-Wl,-rpath,/opt/ndn-base/lib',
              '-lonnx', '-lonnx_proto', '-lprotobuf', '-lndn-cxx', '-ldl', *flags, '-o', str(root / 'probe'))
        check(str(root / 'probe'))
        check('/usr/bin/gst-launch-1.0', '-q', 'fakesrc', 'num-buffers=1', '!', 'fakesink')
        (root / 'probe.rs').write_text('fn main() { assert_eq!(2 + 2, 4); }\n')
        check('/opt/rust/bin/rustc', str(root / 'probe.rs'), '-o', str(root / 'rust-probe'))
        check(str(root / 'rust-probe'))
    check('/opt/venv/bin/python', '-c',
          'import importlib.metadata as m, sysconfig; from pathlib import Path; '
          'import pybind11, pygtrie, aenum; from ndn.encoding import Name; '
          'from Cryptodome.Cipher import AES; '
          'expected={"pybind11":"2.13.6","python-ndn":"0.3","pygtrie":"2.5.0",'
          '"aenum":"3.1.17","pycryptodomex":"3.23.0"}; '
          'assert all(m.version(n)==v for n,v in expected.items()); '
          'assert Path(sysconfig.get_path("include"),"Python.h").is_file(); '
          'assert Name.to_str(Name.from_str("/sdk/probe"))=="/sdk/probe"; '
          'c=AES.new(bytes(16),AES.MODE_ECB); assert c.decrypt(c.encrypt(bytes(16)))==bytes(16)')
    print(json.dumps({'status': 'PASS', 'scope': 'BASE_DEPENDENCY_SDK',
                      'manifestSha256': digest(MANIFEST), 'artifacts': len(body['artifacts'])}))


if __name__ == '__main__':
    if sys.argv[1:] == ['prepare']:
        prepare()
    elif sys.argv[1:] == ['verify']:
        verify()
    elif sys.argv[1:] == ['complete-tokenizer']:
        complete_tokenizer()
    elif sys.argv[1:] == ['complete-nac']:
        complete_nac()
    else:
        raise SystemExit('usage: dependency-sdk.py prepare|verify|complete-tokenizer|complete-nac')
