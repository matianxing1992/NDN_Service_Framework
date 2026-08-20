# -*- Mode: python; py-indent-offset: 4; indent-tabs-mode: nil; coding: utf-8; -*-

from waflib import Context, Logs, Utils
import os, subprocess

VERSION = '0.1.0'
APPNAME = 'ndn-service-framework'
GIT_TAG_PREFIX = 'ndn-service-framework-'


def _resolve_compiler_toolchain(cxx, env=None, expected_root='/usr/bin'):
    """Resolve one compiler/binutils closure, independent of ``PATH``."""
    compiler = os.path.realpath(cxx)
    compiler_dir = os.path.dirname(compiler)
    toolchain_root = os.path.realpath(expected_root)
    if os.path.commonpath([toolchain_root, compiler]) != toolchain_root:
        raise RuntimeError(
            f'configured compiler {compiler} is outside the required toolchain '
            f'root {toolchain_root}; set CXX explicitly or use '
            f'--toolchain-root for an intentional alternate toolchain')

    search_flag = f'-B{toolchain_root}'
    resolved = {
        'compiler_dir': compiler_dir,
        'toolchain_root': toolchain_root,
        'search_flag': search_flag,
    }

    for tool in ('ld', 'ar', 'ranlib', 'nm'):
        result = subprocess.run(
            [compiler, search_flag, f'-print-prog-name={tool}'],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True)
        tool_path = result.stdout.strip()
        if not os.path.isabs(tool_path):
            tool_path = os.path.join(compiler_dir, tool_path)
        tool_path = os.path.realpath(tool_path)
        if not os.path.isfile(tool_path) or not os.access(tool_path, os.X_OK):
            raise RuntimeError(
                f'{compiler} resolved {tool} to a non-executable path: {tool_path}')
        if os.path.commonpath([toolchain_root, tool_path]) != toolchain_root:
            raise RuntimeError(
                f'{compiler} resolved {tool} outside its required toolchain root: '
                f'{tool_path} (expected under {toolchain_root})')
        resolved[tool] = tool_path

    return resolved


def _pin_compiler_toolchain(conf):
    cxx = list(conf.env.CXX or [])
    if not cxx:
        conf.fatal('The C++ compiler was not configured')

    try:
        tools = _resolve_compiler_toolchain(
            cxx[0], env=os.environ.copy(),
            expected_root=conf.options.toolchain_root)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        conf.fatal(f'Unable to establish a closed C++ toolchain: {error}')

    search_flag = tools['search_flag']
    conf.env.CXX = cxx + [search_flag] if search_flag not in cxx else cxx
    link_cxx = list(conf.env.LINK_CXX or cxx)
    conf.env.LINK_CXX = (
        link_cxx + [search_flag] if search_flag not in link_cxx else link_cxx)
    conf.env.AR = [tools['ar']]
    conf.env.RANLIB = [tools['ranlib']]
    conf.env.NM = [tools['nm']]
    conf.env.LD = [tools['ld']]
    conf.env.NDNSF_TOOLCHAIN_ROOT = tools['toolchain_root']
    conf.env.NDNSF_LINKER = tools['ld']

    conf.msg('Closed C++ toolchain',
             f'{os.path.realpath(cxx[0])} -> {tools["ld"]}')

def options(opt):
    opt.load(['compiler_cxx', 'gnu_dirs'])
    opt.load(['default-compiler-flags',
              'coverage', 'sanitizers', 'boost',
              'doxygen'],
             tooldir=['.waf-tools'])
             
    optgrp = opt.add_option_group('ndn-service-framework Options')

    optgrp.add_option('--enable-static', action='store_true', default=False,
                      dest='enable_static', help='Build static library (disabled by default)')
    optgrp.add_option('--disable-static', action='store_false', default=False,
                      dest='enable_static', help='Do not build static library (disabled by default)')

    optgrp.add_option('--enable-shared', action='store_true', default=True,
                      dest='enable_shared', help='Build shared library (enabled by default)')
    optgrp.add_option('--disable-shared', action='store_false', default=True,
                      dest='enable_shared', help='Do not build shared library (enabled by default)')

    optgrp.add_option('--with-examples', action='store_true', default=False,
                      help='Build examples')
    optgrp.add_option('--with-tests', action='store_true', default=False,
                      help='Build unit tests')
    optgrp.add_option('--toolchain-root', default='/usr/bin',
                      help='Required compiler/binutils root (default: /usr/bin)')


def configure(conf):
    conf.start_msg('Building static library')
    if conf.options.enable_static:
        conf.end_msg('yes')
    else:
        conf.end_msg('no', color='YELLOW')
    conf.env.enable_static = conf.options.enable_static

    conf.start_msg('Building shared library')
    if conf.options.enable_shared:
        conf.end_msg('yes')
    else:
        conf.end_msg('no', color='YELLOW')
    conf.env.enable_shared = conf.options.enable_shared

    if not conf.options.enable_shared and not conf.options.enable_static:
        conf.fatal('Either static library or shared library must be enabled')

    conf.load(['compiler_cxx', 'gnu_dirs'])

    # GCC otherwise searches for ld through the build-time PATH even when CXX
    # is an absolute path.  A Linuxbrew directory ahead of /usr/bin can then
    # mix Homebrew ld with system GTK/UAV libraries.  Pin the compiler driver
    # and every binutils program to one directory and fail during configure if
    # that closure cannot be established.
    _pin_compiler_toolchain(conf)
    conf.check(fragment='int main() { return 0; }',
               features='cxx cxxprogram',
               msg='Checking closed C++ linker toolchain')

    # Compiler-flag and dependency tools execute their own link probes, so
    # load them only after the compiler/binutils closure has been pinned.
    conf.load(['default-compiler-flags', 'boost', 'doxygen'])

    conf.env.WITH_EXAMPLES = conf.options.with_examples
    conf.env.WITH_TESTS = conf.options.with_tests

    conf.find_program('dot', mandatory=False)

    # Prefer pkgconf if it's installed, because it gives more correct results
    # on Fedora/CentOS/RHEL/etc. See https://bugzilla.redhat.com/show_bug.cgi?id=1953348
    # Store the result in env.PKGCONFIG, which is the variable used inside check_cfg()
    conf.find_program(['pkgconf', 'pkg-config'], var='PKGCONFIG')

    local_prefix = os.path.join(conf.path.abspath(), '.local-boost171')
    local_pkg_config_path = os.path.join(local_prefix, 'lib', 'pkgconfig')
    pkg_config_paths = []
    if os.environ.get('PKG_CONFIG_PATH'):
        pkg_config_paths.append(os.environ['PKG_CONFIG_PATH'])
    else:
        pkg_config_paths.append(f'{conf.env.LIBDIR}/pkgconfig')
    if os.path.isdir(local_pkg_config_path):
        pkg_config_paths.append(local_pkg_config_path)
        # /usr/local/lib (conf.env.LIBDIR) is already in the system loader
        # cache.  Encoding it ahead of a target's $ORIGIN runpath makes build-
        # tree examples load an older installed framework, and transitively an
        # older NDN-SVS, instead of the libraries they were just linked with.
        conf.env.append_value(
            'LINKFLAGS', f'-Wl,-rpath,{os.path.join(local_prefix, "lib")}')
    pkg_config_path = os.pathsep.join(pkg_config_paths)

    conf.check_cfg(package='libndn-cxx', args=['libndn-cxx >= 0.8.0', '--cflags', '--libs'],
                   uselib_store='NDN_CXX', pkg_config_path=pkg_config_path)

    # The Boost stacktrace/OpenSSL combination used by the pinned Ubuntu
    # toolchain exposes libdl symbols through libndn-cxx's transitive
    # dependencies.  The pkg-config file does not propagate that system
    # library to every executable, so make it part of the common NDN_CXX
    # closure instead of relying on each target to remember it.
    if 'dl' not in conf.env.LIB_NDN_CXX:
        conf.env.LIB_NDN_CXX.append('dl')

    
    conf.check_cfg(package='libndn-svs', args=['libndn-svs >= 0.1.0', '--cflags', '--libs'],
                       uselib_store='NDN_SVS', pkg_config_path=pkg_config_path)

    # An isolated NDN-SVS prefix may coexist with an older installation under
    # /usr/local.  libndn-cxx's pkg-config metadata also contributes
    # /usr/local/include, so the generic use='NDN_CXX NDN_SVS ...' ordering
    # would otherwise compile against the old SVS headers while linking the
    # isolated library.  Put the selected SVS include and runtime directories
    # first to keep headers, link input, and runtime SONAME resolution aligned.
    svs_includes = list(conf.env.INCLUDES_NDN_SVS or [])
    conf.env.INCLUDES_NDN_CXX = svs_includes + [
        path for path in list(conf.env.INCLUDES_NDN_CXX or [])
        if path not in svs_includes
    ]
    # The selected SVS prefix may coexist with an older libndn-svs under the
    # same local prefix used by ndn-cxx.  Waf concatenates uselib library
    # paths in ``use`` order; most targets list NDN_CXX before NDN_SVS, so an
    # older ``.local-boost171/lib/libndn-svs.so`` could satisfy ``-lndn-svs``
    # before the selected Experimental library was considered.  Put the
    # selected SVS directory at the front of the common NDN_CXX path set so
    # every target resolves the header/library pair consistently.
    svs_libpaths = list(conf.env.LIBPATH_NDN_SVS or [])
    conf.env.LIBPATH_NDN_CXX = svs_libpaths + [
        path for path in list(conf.env.LIBPATH_NDN_CXX or [])
        if path not in svs_libpaths
    ]
    svs_rpaths = [f'-Wl,-rpath,{path}' for path in list(conf.env.LIBPATH_NDN_SVS or [])]
    conf.env.LINKFLAGS = svs_rpaths + [
        flag for flag in list(conf.env.LINKFLAGS or [])
        if flag not in svs_rpaths
    ]

    conf.check(features='cxx cxxprogram', lib=['sqlite3'], cflags=['-Wall'], defines=['var=foo'], uselib_store='sqlite3')
    # OpenSSL on the current Debian/Brew toolchain exposes libdl symbols
    # without propagating -ldl through its pkg-config metadata. Keep this
    # explicit for test and example linkers instead of relying on ld's
    # transitive-dependency behavior.
    conf.check(features='cxx cxxprogram', lib=['dl'], uselib_store='DL')


    conf.check_cfg(package='libnac-abe', args=['--cflags', '--libs'], uselib_store='NAC-ABE',
                   pkg_config_path=pkg_config_path)

    conf.check_cfg(package='openssl', args=['--cflags', '--libs'], uselib_store='OPENSSL',
                   pkg_config_path=pkg_config_path)
    
    conf.check_cfg(package='ndnsd', args=['--cflags', '--libs'], uselib_store='NDNSD',
                   pkg_config_path=pkg_config_path)

    # protobuf
    conf.check_cfg(package="protobuf", uselib_store="PROTOBUF", 
            args=['--cflags', '--libs'])
    conf.find_program('protoc', var='PROTOC')

    # MAVSDK libmavsdk-dev_1.4.16
    #conf.check_cfg(package="mavsdk", uselib_store="MAVSDK", 
    #        args=['--cflags', '--libs'])
    
    # gtkmm-3.0
    conf.check_cfg(package="gtkmm-3.0", uselib_store="gtkmm", 
            args=['--cflags', '--libs'], pkg_config_path=pkg_config_path)

    conf.check_cfg(package='onnxruntime', args=['--cflags', '--libs'],
                   uselib_store='ONNXRUNTIME', mandatory=False,
                   pkg_config_path=pkg_config_path)
    conf.env.HAVE_ONNXRUNTIME_CPP = bool(
        conf.env.CXXFLAGS_ONNXRUNTIME or
        conf.env.INCLUDES_ONNXRUNTIME or
        conf.env.LIB_ONNXRUNTIME or
        conf.env.LIBPATH_ONNXRUNTIME)

    conf.check_cfg(package='gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0',
                   args=['--cflags', '--libs'], uselib_store='GSTREAMER',
                   mandatory=False, pkg_config_path=pkg_config_path)
    # Debian's GStreamer .pc files expose GLib/GObject as direct libraries,
    # while the linker used in this environment does not automatically close
    # their gmodule/libffi/PCRE dependency chain. Keep these optional so a
    # minimal host can still configure, but make the transitive closure
    # explicit for the unit-test executable when the libraries are present.
    for library, store in [('gmodule-2.0', 'GMODULE'),
                           ('ffi', 'FFI'),
                           ('pcre', 'PCRE')]:
        conf.check(features='cxx cxxprogram', lib=[library],
                   uselib_store=store, mandatory=False)
    conf.env.HAVE_GSTREAMER = bool(
        conf.env.CXXFLAGS_GSTREAMER or conf.env.INCLUDES_GSTREAMER or
        conf.env.LIB_GSTREAMER or conf.env.LIBPATH_GSTREAMER)

    boost_libs = ['system', 'filesystem']
    if conf.env.WITH_TESTS:
        boost_libs.append('unit_test_framework')

    conf.check_boost(lib=boost_libs, mt=True)

    conf.check_compiler_flags()

    # Loading "late" to prevent tests from being compiled with profiling flags
    conf.load('coverage')
    conf.load('sanitizers')

    # If there happens to be a static library, waf will put the corresponding -L flags
    # before dynamic library flags.  This can result in compilation failure when the
    # system has a different version of the ndn-svs library installed.
    conf.env.prepend_value('STLIBPATH', ['.'])

    conf.define_cond('HAVE_TESTS', conf.env.WITH_TESTS)
    conf.define_cond('HAVE_ONNXRUNTIME_CPP', conf.env.HAVE_ONNXRUNTIME_CPP)
    conf.define_cond('HAVE_GSTREAMER', conf.env.HAVE_GSTREAMER)
    if conf.env.HAVE_ONNXRUNTIME_CPP:
        conf.env.append_value('DEFINES', ['NDNSF_DI_ENABLE_ONNXRUNTIME_CPP'])
    # The config header will contain all defines that were added using conf.define()
    # or conf.define_cond().  Everything that was added directly to conf.env.DEFINES
    # will not appear in the config header, but will instead be passed directly to the
    # compiler on the command line.
    conf.write_config_header('config.hpp')

def build(bld):
    if bld.env.HAVE_GSTREAMER:
        bld.program(
            target='uav-video-pipeline-probe',
            source='NDNSF-UAV-APP/tools/uav_video_pipeline_probe.cpp',
            use='GSTREAMER', install_path=None)

    libndn_service_framework = dict(
        target='ndn-service-framework',
        vnum=VERSION,
        cnum=VERSION,
        source=bld.path.ant_glob('ndn-service-framework/**/*.cpp'),
        use='NDN_SVS NDN_CXX BOOST PROTOBUF NAC-ABE OPENSSL DL',
        includes='ndn-service-framework .',
        export_includes='ndn-service-framework .',
        install_path='${LIBDIR}')

    if bld.env.enable_shared:
        bld.shlib(features="c cshlib",name='ndn-service-framework',
                  **libndn_service_framework)

    if bld.env.enable_static:
        bld.stlib(name='ndn-service-framework-static' if bld.env.enable_shared else 'ndn-service-framework',
                  **libndn_service_framework)

    if bld.env.WITH_TESTS:
        bld.recurse('tests')

    # Spec 111 ownership targets. These object groups keep the mechanism Core
    # and optional model adapters physically distinct without changing the
    # existing executable/link ABI during the compatibility window.
    di_core_sources = bld.path.ant_glob(
        'NDNSF-DistributedInference/cpp/ndnsf-di/*.cpp',
        excl=[
            'NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.cpp',
            'NDNSF-DistributedInference/cpp/ndnsf-di/QwenGenerationSession.cpp',
        ])
    bld.objects(target='ndnsf-di-core-objects', source=di_core_sources,
                includes=['.', 'ndn-service-framework'],
                use='NDN_CXX NDN_SVS PROTOBUF NAC-ABE NDNSD BOOST OPENSSL',
                cxxflags=['-fPIC'])
    bld.objects(
        target='ndnsf-di-adapter-onnx-objects',
        source=bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/adapters/onnx/*.cpp'),
        includes=['.', 'ndn-service-framework'],
        use='NDN_CXX BOOST ONNXRUNTIME', cxxflags=['-fPIC'])
    bld.objects(
        target='ndnsf-di-adapter-qwen-objects',
        source=bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/adapters/qwen/*.cpp'),
        includes=['.', 'ndn-service-framework'],
        use='BOOST', cxxflags=['-fPIC'])

    bld.recurse('NDNSF-DistributedRepo')

    bld.recurse('examples')

    headers = bld.path.ant_glob('ndn-service-framework/**/*.hpp')
    bld.install_files('${INCLUDEDIR}', headers, relative_trick=True)

    bld.install_files('${INCLUDEDIR}/ndn-service-framework',
                      bld.path.find_resource('config.hpp'))

    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/ndnsf-di',
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/ndnsf-di/*.hpp'))
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/adapters/onnx',
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/onnx/*.hpp'))
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/adapters/qwen',
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/qwen/*.hpp'))

    bld(features='subst',
        source='libndn-service-framework.pc.in',
        target='libndn-service-framework.pc',
        install_path='${LIBDIR}/pkgconfig',
        VERSION=VERSION)
