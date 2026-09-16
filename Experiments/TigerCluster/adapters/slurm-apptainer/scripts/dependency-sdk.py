#!/usr/bin/env python3
"""Complete and verify external dependencies in an existing base container."""
import hashlib
import json
import os
import re
import shlex
from pathlib import Path
import shutil
import subprocess
import sys

BASE = Path('/opt/ndn-base')
MANIFEST = BASE / 'manifest/dependency-sdk.json'


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
    for name, archive in [('onnx', 'native/onnx.tar'), ('nac-abe', 'source/nacAbe.tar'),
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
    check('/usr/bin/cmake', '-S', str(sources / 'nac-abe'), '-B', '/tmp/sdk-nac-build',
          '-DCMAKE_BUILD_TYPE=Release', '-DHAVE_TESTS=OFF', '-DBUILD_EXAMPLES=OFF',
          '-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_CXX_FLAGS=' + env['CXXFLAGS'],
          '-DCMAKE_INSTALL_PREFIX=/opt/ndn-base', '-DCMAKE_INSTALL_LIBDIR=lib',
          '-DCMAKE_PREFIX_PATH=/opt/ndn-base', '-DCMAKE_LIBRARY_PATH=/opt/ndn-base/lib',
          '-DCMAKE_INSTALL_RPATH=$ORIGIN', '-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF')
    check('/usr/bin/cmake', '--build', '/tmp/sdk-nac-build', '--parallel', '4')
    check('/usr/bin/cmake', '--install', '/tmp/sdk-nac-build')
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
    for name in ['onnx', 'nac-abe', 'ndn-sd']:
        shutil.rmtree(sources / name)
    for path in ['/tmp/sdk-onnx-build', '/tmp/sdk-nac-build', '/var/lib/apt/lists']:
        shutil.rmtree(path)
    shutil.copy2(__file__, BASE / 'manifest/dependency-sdk.py')
    artifacts = {}
    for directory in [BASE / 'lib', BASE / 'include', BASE / 'sdk/sources', Path('/opt/onnx'),
                      Path('/opt/rust'), Path('/opt/cargo-vendor'), Path('/opt/cargo-home')]:
        for path in directory.rglob('*'):
            if path.is_file():
                artifacts[str(path)] = digest(path)
    MANIFEST.write_text(json.dumps({'schema': 'ndnsf-base-sdk-v1', 'inputs': expected,
        'artifacts': artifacts, 'packages': run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n'),
        'status': 'BUILT_NOT_TESTED'}, indent=2) + '\n')
    verify()


def verify():
    body = json.loads(MANIFEST.read_text())
    assert body['schema'] == 'ndnsf-base-sdk-v1'
    assert run('/usr/bin/dpkg-query', '-W', '-f=${Package}=${Version}\n') == body['packages'], 'SDK_PACKAGES_CHANGED'
    for name, value in body['artifacts'].items():
        assert digest(name) == value, 'SDK_ARTIFACT_CHANGED:' + name
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
    else:
        raise SystemExit('usage: dependency-sdk.py prepare|verify')
