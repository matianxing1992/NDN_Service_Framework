# -*- Mode: python; py-indent-offset: 4; indent-tabs-mode: nil; coding: utf-8; -*-

from waflib import Context, Logs, Utils
import hashlib
import json
import os
import re
import subprocess

VERSION = '0.1.0'
APPNAME = 'ndn-service-framework'
GIT_TAG_PREFIX = 'ndn-service-framework-'

# Host builds consume one installed dependency closure.  The host ONNX Runtime
# SDK is deliberately installed under /opt because it is a versioned system
# SDK; all other host NDNSF dependencies use /usr or /usr/local.  Container
# builds may use their declared SDK roots only after the image sets the explicit
# NDNSF_CONTAINER_BUILD marker.  Checkout, /tmp, .codex-tmp and build-work
# prefixes are never valid dependency inputs.
HOST_GLOBAL_DEPENDENCY_PREFIXES = (
    '/usr', '/usr/local', '/opt/onnxruntime', '/opt/onnxruntime-1.26.0')
CONTAINER_GLOBAL_DEPENDENCY_PREFIXES = (
    '/opt/ndn-base', '/opt/onnx', '/opt/ndnsf-stage', '/opt/ndnsf-di')
# The development host has one supported Boost pair.  Keep this explicit so
# Waf cannot silently select a checkout/staging copy or a second same-version
# installation through BOOST_ROOT/BOOST_INCLUDEDIR/BOOST_LIBRARYDIR.
HOST_BOOST_INCLUDE_DIR = '/usr/include'
HOST_BOOST_LIBRARY_DIR = '/usr/lib/x86_64-linux-gnu'
HOST_BOOST_VERSION = 107100
HOST_DEPENDENCY_IDENTITY_FILE = (
    '/usr/local/share/ndnsf/global-dependency-identity.json')
HOST_DEPENDENCY_LIBRARIES = (
    'libndn-cxx.so', 'libndnsd.so', 'libndn-svs.so', 'libnac-abe.so',
    'libopenabe.so', 'libonnxruntime.so')


def _global_dependency_prefixes():
    roots = list(HOST_GLOBAL_DEPENDENCY_PREFIXES)
    if os.environ.get('NDNSF_CONTAINER_BUILD') == '1':
        roots.extend(CONTAINER_GLOBAL_DEPENDENCY_PREFIXES)
    return tuple(roots)


def _require_global_dependency_prefix(path, owner):
    resolved = os.path.realpath(path)
    roots = _global_dependency_prefixes()
    if not any(resolved == root or resolved.startswith(root + os.sep)
               for root in roots):
        allowed = ', '.join(roots)
        raise RuntimeError(
            f'{owner} must use an installed global dependency root; '
            f'rejected {resolved}. Install it under /usr/local or the '
            'declared container SDK root instead of using a temporary '
            f'checkout prefix (allowed roots: {allowed})')
    return resolved


def _validate_boost_selection(conf):
    """Pin Boost headers and libraries to the host's installed system pair."""
    configured_include = str(getattr(conf.options, 'boost_includes', '') or '').strip()
    configured_lib = str(getattr(conf.options, 'boost_libs', '') or '').strip()
    environment_values = {
        'BOOST_ROOT': os.environ.get('BOOST_ROOT', '').strip(),
        'BOOST_INCLUDEDIR': os.environ.get('BOOST_INCLUDEDIR', '').strip(),
        'BOOST_LIBRARYDIR': os.environ.get('BOOST_LIBRARYDIR', '').strip(),
    }
    requested = {
        'BOOST_INCLUDEDIR': configured_include or environment_values['BOOST_INCLUDEDIR'],
        'BOOST_LIBRARYDIR': configured_lib or environment_values['BOOST_LIBRARYDIR'],
    }
    for owner, value, expected in (
            ('--boost-includes/BOOST_INCLUDEDIR', requested['BOOST_INCLUDEDIR'],
             HOST_BOOST_INCLUDE_DIR),
            ('--boost-libs/BOOST_LIBRARYDIR', requested['BOOST_LIBRARYDIR'],
             HOST_BOOST_LIBRARY_DIR)):
        if value and os.path.realpath(value) != os.path.realpath(expected):
            conf.fatal(
                f'{owner} selected {os.path.realpath(value)}; this host requires '
                f'the installed system Boost pair {expected}. Install/rebuild '
                'Boost globally instead of selecting a checkout or temporary prefix')
    if environment_values['BOOST_ROOT']:
        root = os.path.realpath(environment_values['BOOST_ROOT'])
        if root != '/usr':
            conf.fatal(
                'BOOST_ROOT selected a non-system path: ' + root +
                '; the host Boost pair is /usr/include and '
                '/usr/lib/x86_64-linux-gnu; install/rebuild Boost globally '
                'and unset BOOST_ROOT')

    include_dir = os.path.realpath(HOST_BOOST_INCLUDE_DIR)
    library_dir = os.path.realpath(HOST_BOOST_LIBRARY_DIR)
    for owner, path in (('Boost headers', include_dir),
                        ('Boost libraries', library_dir)):
        try:
            _require_global_dependency_prefix(path, owner)
        except RuntimeError as error:
            conf.fatal(str(error))
        if not os.path.isdir(path):
            conf.fatal(f'Installed {owner.lower()} directory is missing: {path}')
    version_header = os.path.join(include_dir, 'boost', 'version.hpp')
    if not os.path.isfile(version_header):
        conf.fatal(
            f'Installed Boost headers are missing: {version_header}; '
            'install/rebuild the system Boost pair before configuring')
    return include_dir, library_dir


def _validate_boost_libraries(conf, library_dir, libraries):
    """Require each selected Boost SONAME to stay in the pinned system dir."""
    for library in libraries:
        link_path = os.path.join(library_dir, f'libboost_{library}.so')
        resolved = os.path.realpath(link_path)
        if not os.path.isfile(link_path) or not os.path.isfile(resolved):
            conf.fatal(
                f'Installed Boost library is missing: {link_path}; '
                'install/rebuild the system Boost pair before configuring')
        try:
            inside = os.path.commonpath([library_dir, resolved]) == library_dir
        except ValueError:
            inside = False
        if not inside:
            conf.fatal(
                f'Boost library escapes the pinned system directory: '
                f'{link_path} -> {resolved}')
        expected_soname = f'libboost_{library}.so.1.71.0'
        result = subprocess.run(
            ['/usr/bin/readelf', '-d', resolved],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode != 0 or (
                f'Library soname: [{expected_soname}]' not in result.stdout):
            conf.fatal(
                f'Boost library has the wrong SONAME: {resolved}; expected '
                f'{expected_soname}. Install/rebuild the system Boost pair')


def _validate_global_dependency_identity(conf):
    """Match installed dependency DSOs to the installer-issued identity receipt."""
    if os.environ.get('NDNSF_CONTAINER_BUILD') == '1':
        return
    receipt_path = HOST_DEPENDENCY_IDENTITY_FILE
    if not os.path.isfile(receipt_path):
        conf.fatal(
            f'Global dependency identity receipt is missing: {receipt_path}; '
            'run install_ndnsf_stack.sh to install/rebuild the global closure')
    try:
        with open(receipt_path, encoding='utf-8') as stream:
            receipt = json.load(stream)
    except (OSError, ValueError) as error:
        conf.fatal(f'Cannot read global dependency identity receipt: {error}')
    libraries = receipt.get('libraries', {})
    if receipt.get('schema') != 1 or not isinstance(libraries, dict):
        conf.fatal(f'Invalid global dependency identity receipt: {receipt_path}')
    for name in HOST_DEPENDENCY_LIBRARIES:
        entry = libraries.get(name)
        if not isinstance(entry, dict):
            conf.fatal(f'Global dependency identity is missing {name}: {receipt_path}')
        path = entry.get('path')
        expected_realpath = entry.get('realpath')
        expected_digest = entry.get('sha256')
        expected_soname = entry.get('soname')
        if (not all(isinstance(value, str) and value for value in
                    (path, expected_realpath, expected_digest)) or
                not isinstance(expected_soname, str)):
            conf.fatal(f'Invalid identity entry for {name}: {receipt_path}')
        if name == 'libonnxruntime.so':
            allowed_paths = (
                '/opt/onnxruntime/lib/libonnxruntime.so',
                '/opt/onnxruntime-1.26.0/lib/libonnxruntime.so')
        else:
            allowed_paths = (f'/usr/local/lib/{name}',)
        if path not in allowed_paths:
            conf.fatal(
                f'Global dependency identity path is outside the canonical '
                f'root for {name}: {path}')
        actual_realpath = os.path.realpath(path)
        allowed_roots = ('/opt/onnxruntime', '/opt/onnxruntime-1.26.0') \
            if name == 'libonnxruntime.so' else ('/usr/local',)
        if not any(actual_realpath == root or
                   actual_realpath.startswith(root + os.sep)
                   for root in allowed_roots):
            conf.fatal(
                f'Global dependency realpath is outside the canonical root '
                f'for {name}: {actual_realpath}')
        if actual_realpath != expected_realpath or not os.path.isfile(actual_realpath):
            conf.fatal(
                f'Global dependency realpath changed for {name}: '
                f'{path} -> {actual_realpath}; rerun the global installer')
        actual_digest = hashlib.sha256(open(actual_realpath, 'rb').read()).hexdigest()
        if actual_digest != expected_digest:
            conf.fatal(
                f'Global dependency digest changed for {name}: '
                f'{actual_digest} != {expected_digest}; rerun the global installer')
        result = subprocess.run(
            ['/usr/bin/readelf', '-d', actual_realpath],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        match = re.search(r'Library soname: \[([^]]+)\]', result.stdout)
        actual_soname = match.group(1) if match else ''
        if result.returncode != 0 or actual_soname != expected_soname:
            conf.fatal(
                f'Global dependency SONAME changed for {name}: '
                f'expected {expected_soname}; rerun the global installer')


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

    # Clang keeps its driver/binutils lookup relative to its LLVM resource
    # directory.  Passing ``-B/usr`` to the Debian clang symlink can therefore
    # still select Linuxbrew's ``ld``; use the resolved clang directory as the
    # driver search prefix while retaining /usr as the allowed system closure.
    search_root = (compiler_dir if os.path.basename(compiler).startswith('clang')
                   else toolchain_root)
    search_flag = f'-B{search_root}'
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


def _ensure_tokenizer_bridge(conf):
    """Use the installed Rust tokenizer bridge; never build from a checkout."""
    configured = os.environ.get('NDNSF_TOKENIZER_BRIDGE_ARCHIVE', '').strip()
    candidates = ([configured] if configured else []) + [
        '/usr/local/lib/libndnsf_tokenizer_bridge.a',
        '/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a']
    archive = next((os.path.realpath(path) for path in candidates
                    if path and os.path.isfile(path)), None)
    if archive is None:
        conf.fatal(
            'Installed Rust tokenizer bridge is missing; install '
            'libndnsf_tokenizer_bridge.a under /usr/local/lib or the '
            'declared container SDK lib directory before configuring')
    _require_global_dependency_prefix(os.path.dirname(archive),
                                      'tokenizer bridge')
    target_dir = os.path.dirname(archive)
    conf.env.STLIB_TOKENIZER_BRIDGE = ['ndnsf_tokenizer_bridge']
    conf.env.STLIBPATH_TOKENIZER_BRIDGE = [target_dir]
    conf.env.NDNSF_TOKENIZER_BRIDGE_ARCHIVE = archive
    conf.msg('Installed Rust tokenizer staticlib', archive)


def _pin_compiler_toolchain(conf):
    """Record the closed compiler/binutils closure for every later task.

    configure() pins the driver with _resolve_compiler_toolchain (search
    flag, toolchain root); this mirror records the resolved tools in the
    build env so the NDNSF_LINKER used by later tasks is the exact ld from
    the closed toolchain, not whatever the default linker search yields.
    """
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
    optgrp.add_option('--install-experiment-fixtures', action='store_true', default=False,
                      help='Install explicitly selected native experiment fixtures')
    optgrp.add_option('--toolchain-root', default='/usr/bin',
                      help='Required compiler/binutils root (default: /usr/bin)')
    optgrp.add_option('--ndn-svs-source-tree', default='',
                      help='Explicit NDN-SVS source tree for an uninstalled build')
    optgrp.add_option('--ndn-svs-build-tree', default='',
                      help='Explicit matching NDN-SVS build tree containing libndn-svs.so')
    optgrp.add_option('--disable-local-dependency-prefix', action='store_true',
                      dest='disable_local_dependency_prefix', default=True,
                      help='Do not add the repository .local-boost171 prefix to dependency discovery (default)')
    optgrp.add_option('--enable-local-dependency-prefix', action='store_false',
                      dest='disable_local_dependency_prefix', default=True,
                      help='Deprecated; global dependency policy rejects this option')
    optgrp.add_option('--nac-abe-prefix', default='/usr/local',
                      help='Installed NAC-ABE root (host default: /usr/local)')
    optgrp.add_option('--onnx-prefix', default='/usr/local',
                      help='Installed ONNX 1.17 full-protobuf root (host default: /usr/local)')
    optgrp.add_option('--allow-container-runtime-rpath', action='store_true',
                      default=False,
                      help='Allow $ORIGIN runtime RPATH for an explicit container SDK build')


def configure(conf):
    # Report missing SDKs together before receipt/ABI validation stops at the
    # first error. This is read-only; dependencies own their installation.
    import runpy
    inventory = runpy.run_path(os.path.join(conf.path.abspath(), 'scripts',
                                           'configure_dependencies.py'))
    missing, optional = inventory['check_dependencies'](
        nac_prefix=conf.options.nac_abe_prefix,
        onnx_prefix=conf.options.onnx_prefix)
    for warning in optional:
        Logs.warn(warning)
    if missing:
        conf.fatal('Missing configure dependencies:\n  - ' +
                   '\n  - '.join(missing) +
                   '\nUse ./configure.sh for OS packages; install NDN libraries '
                   'with their own projects. See docs/configure-dependencies.md.')
    _validate_global_dependency_identity(conf)
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

    # Keep the configured Waf build tree ahead of the installed NDNSF copy for
    # in-tree tests and examples. ``gnu_dirs`` adds the install libdir to the
    # global RPATH when the prefix is /usr/local; that makes a freshly linked
    # selector load an older same-SONAME library from /usr/local before its
    # own build-tree target. The installed global libdir is already in the
    # host loader cache, while target-local ``rpath`` entries remain explicit.
    installed_lib_rpath = f'-Wl,-rpath,{os.path.realpath(conf.env.LIBDIR)}'
    conf.env.LINKFLAGS = [flag for flag in list(conf.env.LINKFLAGS or [])
                          if flag != installed_lib_rpath]

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
    conf.env.INSTALL_EXPERIMENT_FIXTURES = conf.options.install_experiment_fixtures
    if conf.env.INSTALL_EXPERIMENT_FIXTURES and not (
            conf.env.WITH_TESTS and conf.env.WITH_EXAMPLES):
        conf.fatal('--install-experiment-fixtures requires --with-tests and --with-examples')

    conf.find_program('dot', mandatory=False)

    # Prefer pkgconf if it's installed, because it gives more correct results
    # on Fedora/CentOS/RHEL/etc. See https://bugzilla.redhat.com/show_bug.cgi?id=1953348
    # Store the result in env.PKGCONFIG, which is the variable used inside check_cfg()
    conf.find_program(['pkgconf', 'pkg-config'], var='PKGCONFIG')

    local_prefix = os.path.join(conf.path.abspath(), '.local-boost171')
    local_pkg_config_path = os.path.join(local_prefix, 'lib', 'pkgconfig')

    if not conf.options.disable_local_dependency_prefix:
        conf.fatal('The retired .local-boost171 dependency tree is not allowed; '
                   'install one global host dependency closure instead')

    def reject_non_global_paths(paths, owner, allow_origin=False):
        """Reject checkout, temporary, and undeclared dependency roots."""
        if isinstance(paths, str):
            paths = [paths]
        for value in paths:
            if not value:
                continue
            raw = os.path.expanduser(str(value))
            # Container APPs intentionally use a relative loader token such as
            # ``$ORIGIN/../../lib``.  It is meaningful only in an explicit
            # runtime RPATH and is never accepted from dependency pkg-config
            # flags, where it could hide an arbitrary checkout.
            if allow_origin and (raw == '$ORIGIN' or raw.startswith('$ORIGIN/')):
                continue
            real = os.path.realpath(raw)
            try:
                _require_global_dependency_prefix(real, owner)
            except RuntimeError as error:
                conf.fatal(str(error))

    def check_dependency_paths(owner, *keys):
        for key in keys:
            reject_non_global_paths(getattr(conf.env, key, []) or [], owner)

    def dependency_flag_paths(flags):
        """Extract filesystem paths from pkg-config/compiler/linker flags."""
        paths = []
        if isinstance(flags, str):
            flags = [flags]
        values = [str(flag) for flag in flags]
        index = 0
        while index < len(values):
            value = values[index]
            if value in ('-I', '-isystem', '-L') and index + 1 < len(values):
                paths.append(values[index + 1])
                index += 2
                continue
            for prefix in ('-I', '-isystem', '-L'):
                if value.startswith(prefix) and value != prefix:
                    paths.append(value[len(prefix):])
                    break
            if value.startswith('-Wl,'):
                parts = value[4:].split(',')
                for part_index, part in enumerate(parts):
                    if part in ('-rpath', '-rpath-link', '-R', '-L',
                                '--rpath', '--rpath-link') and part_index + 1 < len(parts):
                        paths.append(parts[part_index + 1])
                    elif part.startswith('-rpath='):
                        paths.append(part.split('=', 1)[1])
                    elif part.startswith(('-rpath-link=', '--rpath=', '--rpath-link=')):
                        paths.append(part.split('=', 1)[1])
                    elif part.startswith('-R') and part != '-R':
                        paths.append(part[2:])
                    elif part.startswith('-L') and part != '-L':
                        paths.append(part[2:])
            index += 1
        return paths

    def reject_non_global_link_flags(flags, owner, allow_origin=False):
        reject_non_global_paths(dependency_flag_paths(flags), owner,
                                allow_origin=allow_origin)

    def check_dependency_link_flags(owner, *keys):
        for key in keys:
            reject_non_global_link_flags(
                getattr(conf.env, key, []) or [], owner)

    pkg_config_paths = []
    pkg_config_libdir = os.environ.get('PKG_CONFIG_LIBDIR', '')
    if pkg_config_libdir:
        for entry in pkg_config_libdir.split(os.pathsep):
            if entry:
                _require_global_dependency_prefix(entry, 'PKG_CONFIG_LIBDIR')
                if not os.path.isdir(entry):
                    conf.fatal('PKG_CONFIG_LIBDIR contains a missing directory: ' + entry)
    if os.environ.get('PKG_CONFIG_PATH'):
        pkg_config_paths.append(os.environ['PKG_CONFIG_PATH'])
    else:
        pkg_config_paths.append(f'{conf.env.LIBDIR}/pkgconfig')
    if (not conf.options.disable_local_dependency_prefix
            and os.path.isdir(local_pkg_config_path)):
        pkg_config_paths.append(local_pkg_config_path)
        # /usr/local/lib (conf.env.LIBDIR) is already in the system loader
        # cache.  Encoding it ahead of a target's $ORIGIN runpath makes build-
        # tree examples load an older installed framework, and transitively an
        # older NDN-SVS, instead of the libraries they were just linked with.
        conf.env.append_value(
            'LINKFLAGS', f'-Wl,-rpath,{os.path.join(local_prefix, "lib")}')
    for configured_path in pkg_config_paths:
        for entry in configured_path.split(os.pathsep):
            if entry:
                _require_global_dependency_prefix(entry, 'PKG_CONFIG_PATH')
                if not os.path.isdir(entry):
                    conf.fatal('PKG_CONFIG_PATH contains a missing directory: ' + entry)
    nac_abe_prefix = _require_global_dependency_prefix(
        conf.options.nac_abe_prefix, 'NAC-ABE')
    nac_header = os.path.join(nac_abe_prefix, 'include', 'nac-abe', 'consumer.hpp')
    nac_library = os.path.join(nac_abe_prefix, 'lib', 'libnac-abe.so')
    nac_pc = os.path.join(nac_abe_prefix, 'lib', 'pkgconfig')
    if not os.path.isfile(nac_header):
        conf.fatal(f'Installed NAC-ABE header is missing: {nac_header}')
    if not os.path.isfile(nac_library):
        conf.fatal(f'Installed NAC-ABE library is missing: {nac_library}')
    if os.path.isdir(nac_pc):
        pkg_config_paths.insert(0, nac_pc)
    # Spec 182 unified the DI ONNX world on the official 1.17 full-protobuf
    # build (ONNX_USE_LITE_PROTO=OFF).  The vendored lite trio is gone, so the
    # onnx adapter sources always need these headers and archives; fail early
    # from the installed global root instead of letting every later target die
    # on a missing header.
    onnx_prefix = _require_global_dependency_prefix(
        conf.options.onnx_prefix, 'ONNX')
    onnx_checker = os.path.join(onnx_prefix, 'include', 'onnx', 'checker.h')
    onnx_shape_inference = os.path.join(
        onnx_prefix, 'include', 'onnx', 'shape_inference', 'implementation.h')
    onnx_lib = os.path.join(onnx_prefix, 'lib', 'libonnx.a')
    onnx_proto_lib = os.path.join(onnx_prefix, 'lib', 'libonnx_proto.a')
    for onnx_required in [onnx_checker, onnx_shape_inference,
                          onnx_lib, onnx_proto_lib]:
        if not os.path.isfile(onnx_required):
            conf.fatal(f'Installed ONNX file is missing: {onnx_required}')
    conf.env.INCLUDES_ONNX = [os.path.join(onnx_prefix, 'include')]
    conf.env.LIBPATH_ONNX = [os.path.join(onnx_prefix, 'lib')]
    conf.env.LIB_ONNX = ['onnx', 'onnx_proto']
    conf.env.CXXFLAGS_ONNX = ['-DONNX_ML=1', '-DONNX_NAMESPACE=onnx']
    conf.msg('ONNX full-protobuf prefix', onnx_prefix)
    pkg_config_path = os.pathsep.join(pkg_config_paths)

    conf.check_cfg(package='libndn-cxx', args=['libndn-cxx >= 0.8.0', '--cflags', '--libs'],
                   uselib_store='NDN_CXX', pkg_config_path=pkg_config_path)
    check_dependency_paths('libndn-cxx', 'INCLUDES_NDN_CXX', 'LIBPATH_NDN_CXX')
    check_dependency_link_flags('libndn-cxx', 'LINKFLAGS_NDN_CXX')

    # The Boost stacktrace/OpenSSL combination used by the pinned Ubuntu
    # toolchain exposes libdl symbols through libndn-cxx's transitive
    # dependencies.  The pkg-config file does not propagate that system
    # library to every executable, so make it part of the common NDN_CXX
    # closure instead of relying on each target to remember it.
    if 'dl' not in conf.env.LIB_NDN_CXX:
        conf.env.LIB_NDN_CXX.append('dl')

    
    conf.check_cfg(package='libndn-svs', args=['libndn-svs >= 0.1.0', '--cflags', '--libs'],
                       uselib_store='NDN_SVS', pkg_config_path=pkg_config_path)
    check_dependency_paths('libndn-svs', 'INCLUDES_NDN_SVS', 'LIBPATH_NDN_SVS')
    check_dependency_link_flags('libndn-svs', 'LINKFLAGS_NDN_SVS')

    # Experimental NDN-SVS development commonly precedes installation.  A
    # header-only override is unsafe: it compiles against the new API but may
    # link /usr/local's older SONAME, failing only at the final executable.
    # Accept only an explicit source/build pair and replace both halves of the
    # pkg-config result atomically.
    svs_source_tree = os.path.realpath(conf.options.ndn_svs_source_tree) \
        if conf.options.ndn_svs_source_tree else ''
    svs_build_tree = os.path.realpath(conf.options.ndn_svs_build_tree) \
        if conf.options.ndn_svs_build_tree else ''
    if bool(svs_source_tree) != bool(svs_build_tree):
        conf.fatal('--ndn-svs-source-tree and --ndn-svs-build-tree must be supplied together')
    if svs_source_tree:
        _require_global_dependency_prefix(svs_source_tree, 'NDN-SVS source')
        _require_global_dependency_prefix(svs_build_tree, 'NDN-SVS build')
        svs_header = os.path.join(svs_source_tree, 'ndn-svs', 'svspubsub.hpp')
        svs_library = os.path.join(svs_build_tree, 'libndn-svs.so')
        if not os.path.isfile(svs_header):
            conf.fatal(f'Explicit NDN-SVS header is missing: {svs_header}')
        if not os.path.isfile(svs_library):
            conf.fatal(f'Explicit NDN-SVS library is missing: {svs_library}')
        # Source headers include the generated build/config.hpp, so both roots
        # are one inseparable header closure.
        conf.env.INCLUDES_NDN_SVS = [svs_source_tree, svs_build_tree]
        conf.env.LIBPATH_NDN_SVS = [svs_build_tree]
        conf.env.LIB_NDN_SVS = ['ndn-svs']
        conf.env.NDNSF_NDN_SVS_SOURCE_TREE = svs_source_tree
        conf.env.NDNSF_NDN_SVS_BUILD_TREE = svs_build_tree
        conf.msg('Explicit NDN-SVS source/build pair',
                 f'{svs_source_tree} -> {svs_library}')

    # A container may explicitly point at its own declared SDK build tree.  A
    # host build normally leaves these options empty and consumes the global
    # installed NDN-SVS pair from pkg-config.
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
    # The build-tree framework is loaded by examples from $ORIGIN/.. .  Do
    # not place the installed system libdir ahead of that runpath: an older
    # /usr/local/libndn-service-framework can otherwise satisfy the SONAME and
    # leave newly linked symbols unresolved at process startup.  The system
    # libdir is already in ld.so.cache; retain explicit non-system SVS paths.
    svs_rpaths = [
        f'-Wl,-rpath,{path}' for path in list(conf.env.LIBPATH_NDN_SVS or [])
        if path != conf.env.LIBDIR
    ]
    conf.env.LINKFLAGS = svs_rpaths + [
        flag for flag in list(conf.env.LINKFLAGS or [])
        if flag not in svs_rpaths
    ]
    if svs_source_tree:
        conf.check(
            fragment='''
#include <ndn-svs/svspubsub.hpp>
int main() {
  auto method = &ndn::svs::SVSPubSub::subscribeToProducerWithCatchUp;
  (void)method;
  return 0;
}
''',
            features='cxx cxxprogram', use='NDN_SVS NDN_CXX',
            msg='Checking explicit NDN-SVS header/library closure')

    conf.check(features='cxx cxxprogram', lib=['sqlite3'], cflags=['-Wall'], defines=['var=foo'], uselib_store='sqlite3')
    # OpenSSL on the current Debian/Brew toolchain exposes libdl symbols
    # without propagating -ldl through its pkg-config metadata. Keep this
    # explicit for test and example linkers instead of relying on ld's
    # transitive-dependency behavior.
    conf.check(features='cxx cxxprogram', lib=['dl'], uselib_store='DL')


    # NAC-ABE is an external project. Its own repository build installs the
    # SDK; this NDNSF Waf graph only consumes the installed contract and never
    # recurses into NAC-ABE sources or builds its targets.
    conf.check_cfg(package='libnac-abe', args=['--cflags', '--libs'], uselib_store='NAC-ABE',
                   pkg_config_path=pkg_config_path)
    check_dependency_paths('libnac-abe', 'INCLUDES_NAC-ABE', 'INCLUDES_NAC_ABE',
                           'LIBPATH_NAC-ABE', 'LIBPATH_NAC_ABE')
    check_dependency_link_flags('libnac-abe', 'LINKFLAGS_NAC-ABE')

    # libndn-cxx commonly contributes /usr/local/include and /usr/local/lib.
    # Keep the installed NAC-ABE header/library pair together even when a
    # container SDK root is selected explicitly.
    if nac_abe_prefix:
        nac_includes = [os.path.join(nac_abe_prefix, 'include')]
        # ``use='NAC-ABE'`` consumes the hyphenated Waf keys generated by
        # check_cfg.  Keep the underscore aliases for diagnostics/backwards
        # compatibility, but update the keys that task generators actually
        # use; otherwise an explicit prefix still falls through to the stale
        # /usr/local headers.
        for key in ('INCLUDES_NAC-ABE', 'INCLUDES_NAC_ABE'):
            # Both spellings are compatibility aliases.  They must describe
            # the same selected prefix; retaining the check_cfg fallback in
            # only the hyphenated key would let a consumer silently compile
            # against a different installed header tree.
            conf.env[key] = list(nac_includes)
        nac_libpaths = [os.path.join(nac_abe_prefix, 'lib')]
        for key in ('LIBPATH_NAC-ABE', 'LIBPATH_NAC_ABE'):
            conf.env[key] = list(nac_libpaths)
        conf.env.INCLUDES_NDN_CXX = nac_includes + [
            path for path in list(conf.env.INCLUDES_NDN_CXX or [])
            if path not in nac_includes]
        conf.env.LIBPATH_NDN_CXX = nac_libpaths + [
            path for path in list(conf.env.LIBPATH_NDN_CXX or [])
            if path not in nac_libpaths]
        nac_libdir = os.path.realpath(os.path.join(nac_abe_prefix, 'lib'))
        nac_rpath = f'-Wl,-rpath,{nac_libdir}'
        # /usr/local is the installed host loader root and is already in the
        # loader cache. Keep it out of build-tree RPATH ordering; an explicit
        # non-installed NAC-ABE prefix still needs its own runtime path.
        if nac_libdir != os.path.realpath(conf.env.LIBDIR):
            conf.env.LINKFLAGS = [nac_rpath] + [
                flag for flag in list(conf.env.LINKFLAGS or []) if flag != nac_rpath]
        else:
            conf.env.LINKFLAGS = [
                flag for flag in list(conf.env.LINKFLAGS or []) if flag != nac_rpath]
        conf.env.NDNSF_NAC_ABE_PREFIX = nac_abe_prefix
        conf.msg('Installed NAC-ABE prefix', nac_abe_prefix)

    # A layered application may link against staged builder inputs, but its
    # published ELF objects must resolve only through their APP-relative path
    # and the immutable base runtime.  When requested, replace every
    # dependency-discovery RPATH accumulated above (SVS, NAC-ABE, local
    # prefixes) with the explicit pair runtime paths.  Link search paths stay
    # intact, so this changes runtime ownership without weakening the build
    # closure.
    runtime_rpath = os.environ.get('NDNSF_RUNTIME_RPATH', '')
    if runtime_rpath:
        runtime_paths = [path for path in runtime_rpath.split(os.pathsep) if path]
        origin_paths = [path for path in runtime_paths
                        if path == '$ORIGIN' or path.startswith('$ORIGIN/')]
        if origin_paths and not conf.options.allow_container_runtime_rpath:
            conf.fatal(
                'NDNSF_RUNTIME_RPATH contains $ORIGIN; pass '
                '--allow-container-runtime-rpath only for a declared container '
                'SDK build, never for a host build')
        if origin_paths and not any(
                path.startswith('/opt/ndn-base/') or path == '/opt/ndn-base'
                for path in runtime_paths):
            conf.fatal(
                'A container $ORIGIN runtime RPATH must also name the declared '
                '/opt/ndn-base SDK root')
        reject_non_global_paths(runtime_paths, 'NDNSF_RUNTIME_RPATH',
                                allow_origin=bool(origin_paths))
        reject_non_global_link_flags(
            [f'-Wl,-rpath,{path}' for path in runtime_paths],
            'NDNSF_RUNTIME_RPATH', allow_origin=True)
        existing_linkflags = [
            flag for flag in list(conf.env.LINKFLAGS or [])
            if not flag.startswith('-Wl,-rpath,')
        ]
        conf.env.LINKFLAGS = [
            *(f'-Wl,-rpath,{path}' for path in runtime_paths),
            *existing_linkflags,
        ]

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

    conf.check_cfg(package='onnxruntime',
                   args=['onnxruntime >= 1.26.0', '--cflags', '--libs'],
                   uselib_store='ONNXRUNTIME', mandatory=True,
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

    # Every dependency-discovery result must remain inside the installed host
    # closure.  Checking only the three primary NDNSF libraries is insufficient:
    # OpenSSL, NDNSD, protobuf, ONNX Runtime, GTK and GStreamer .pc files can
    # inject their own include, library or RPATH flags.
    for uselib in ('NDN_CXX', 'NDN_SVS', 'NAC-ABE', 'NAC_ABE', 'OPENSSL',
                   'NDNSD', 'PROTOBUF', 'ONNXRUNTIME', 'GTKMM', 'GSTREAMER'):
        for key_prefix in ('INCLUDES_', 'LIBPATH_', 'STLIBPATH_',
                           'CXXFLAGS_', 'CFLAGS_', 'LINKFLAGS_'):
            key = key_prefix + uselib
            if key_prefix in ('INCLUDES_', 'LIBPATH_', 'STLIBPATH_'):
                check_dependency_paths(key, key)
            else:
                reject_non_global_link_flags(
                    getattr(conf.env, key, []) or [], key)

    boost_include_dir, boost_library_dir = _validate_boost_selection(conf)
    boost_libs = ['system', 'filesystem']
    if conf.env.WITH_TESTS:
        boost_libs.append('unit_test_framework')

    conf.check_boost(lib=boost_libs, mt=True,
                    includes=boost_include_dir, libs=boost_library_dir)
    if conf.env.BOOST_VERSION_NUMBER != HOST_BOOST_VERSION:
        conf.fatal(
            f'Installed Boost version {conf.env.BOOST_VERSION_NUMBER} is not '
            f'the required host version {HOST_BOOST_VERSION}; install/rebuild '
            'the system Boost pair instead of selecting another prefix')
    selected_boost_includes = [os.path.realpath(path)
                               for path in Utils.to_list(conf.env.INCLUDES_BOOST or [])]
    selected_boost_libpaths = [os.path.realpath(path)
                               for path in Utils.to_list(conf.env.LIBPATH_BOOST or [])]
    if selected_boost_includes != [boost_include_dir]:
        conf.fatal('Waf selected a non-canonical Boost include directory: ' +
                   repr(selected_boost_includes))
    if selected_boost_libpaths != [boost_library_dir]:
        conf.fatal('Waf selected a non-canonical Boost library directory: ' +
                   repr(selected_boost_libpaths))
    _validate_boost_libraries(conf, boost_library_dir, boost_libs)
    conf.env.NDNSF_BOOST_INCLUDE_DIR = boost_include_dir
    conf.env.NDNSF_BOOST_LIBRARY_DIR = boost_library_dir

    conf.check_compiler_flags()

    # Loading "late" to prevent tests from being compiled with profiling flags
    conf.load('coverage')
    conf.load('sanitizers')

    # If there happens to be a static library, waf will put the corresponding -L flags
    # before dynamic library flags.  This can result in compilation failure when the
    # system has a different version of the ndn-svs library installed.
    conf.env.prepend_value('STLIBPATH', ['.'])

    _ensure_tokenizer_bridge(conf)

    # Metadata and file presence do not establish header/library compatibility.
    for label, use, fragment in [
        ('SQLite', 'sqlite3', '#include <sqlite3.h>\nint main(){sqlite3* db=nullptr; int r=sqlite3_open(":memory:",&db); sqlite3_close(db); return r;}'),
        ('OpenSSL', 'OPENSSL', '#include <openssl/evp.h>\nint main(){auto* c=EVP_MD_CTX_new(); EVP_MD_CTX_free(c);}'),
        ('Protobuf', 'PROTOBUF', '#include <google/protobuf/stubs/common.h>\nint main(){GOOGLE_PROTOBUF_VERIFY_VERSION; google::protobuf::ShutdownProtobufLibrary();}'),
        ('ONNX full protobuf', 'ONNX PROTOBUF', '#include <onnx/onnx_pb.h>\nint main(){onnx::ModelProto m; return m.ByteSizeLong() != 0;}'),
        ('ONNX Runtime', 'ONNXRUNTIME', '#include <onnxruntime_c_api.h>\nint main(){return OrtGetApiBase()->GetApi(ORT_API_VERSION) == nullptr;}'),
    ]:
        conf.check(features='cxx cxxprogram', fragment=fragment, use=use,
                   mandatory=True, msg='Checking ' + label + ' compile/link closure')

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
            use='GSTREAMER GMODULE FFI PCRE DL', install_path=None)

    libndn_service_framework = dict(
        target='ndn-service-framework',
        vnum=VERSION,
        cnum=VERSION,
        source=bld.path.ant_glob('ndn-service-framework/**/*.cpp'),
        use='NDN_CXX NDN_SVS BOOST PROTOBUF NAC-ABE NDNSD OPENSSL DL',
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
        includes=['.', 'ndn-service-framework',
                  'NDNSF-DistributedInference/cpp/adapters/onnx'],
        use='NDN_CXX BOOST PROTOBUF ONNX ONNXRUNTIME', cxxflags=['-fPIC'])
    bld.objects(
        target='ndnsf-di-adapter-yolo-objects',
        source=bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/adapters/yolo/*.cpp'),
        includes=['.', 'ndn-service-framework'],
        use='NDN_CXX BOOST', cxxflags=['-fPIC'])
    bld.objects(
        target='ndnsf-di-adapter-qwen-objects',
        source=bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/adapters/qwen/*.cpp'),
        includes=['.', 'ndn-service-framework'],
        use='BOOST', cxxflags=['-fPIC'])

    # One installable native DI library is the link boundary for C++
    # consumers and the optional Python binding.  Keep the source closure
    # identical to the object groups above; adapters remain model-specific
    # while the shared library owns no Python runtime.
    di_library_sources = di_core_sources + bld.path.ant_glob(
        'NDNSF-DistributedInference/cpp/adapters/onnx/*.cpp') + \
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/yolo/*.cpp') + \
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/qwen/*.cpp')
    di_library_use = 'ndn-service-framework NDN_CXX NDN_SVS PROTOBUF ONNX NAC-ABE NDNSD BOOST OPENSSL DL'
    if bld.env.HAVE_ONNXRUNTIME_CPP:
        di_library_use += ' ONNXRUNTIME'
    if bld.env.enable_shared:
        # The pinned Rust tokenizer engine links into the installed DI shared
        # library (spec182 T007-A frozen production integration).
        di_shlib_use = di_library_use + ' TOKENIZER_BRIDGE'
        bld.shlib(name='ndnsf-distributed-inference',
                  target='ndnsf-distributed-inference',
                  source=di_library_sources,
                  use=di_shlib_use,
                  includes=['.', 'ndn-service-framework',
                            'NDNSF-DistributedInference/cpp/adapters/onnx'],
                  export_includes=['.', 'ndn-service-framework',
                                   'NDNSF-DistributedInference/cpp'],
                  install_path='${LIBDIR}',
                  linkflags=['-Wl,-rpath,$ORIGIN'])
    if bld.env.enable_static:
        bld.stlib(name='ndnsf-distributed-inference-static'
                  if bld.env.enable_shared else 'ndnsf-distributed-inference',
                  target='ndnsf-distributed-inference-static'
                  if bld.env.enable_shared else 'ndnsf-distributed-inference',
                  source=di_library_sources,
                  use=di_library_use,
                  includes=['.', 'ndn-service-framework',
                            'NDNSF-DistributedInference/cpp/adapters/onnx'],
                  export_includes=['.', 'ndn-service-framework',
                                   'NDNSF-DistributedInference/cpp'],
                  install_path='${LIBDIR}')

    bld.recurse('NDNSF-DistributedRepo')

    bld.recurse('examples')

    headers = bld.path.ant_glob('ndn-service-framework/**/*.hpp')
    bld.install_files('${INCLUDEDIR}', headers, relative_trick=True)

    bld.install_files('${INCLUDEDIR}/ndn-service-framework',
                      bld.path.find_resource('config.hpp'))

    # Preserve the pre-existing Native* compatibility installation until the
    # T011 caller matrix proves which entries can be retired.  The three new
    # umbrellas below are the stable application/provider/extension boundary;
    # the legacy glob remains explicitly classified as compatibility in
    # contracts/api-exposure.json during this migration window.
    di_installed_headers = [
        'NDNSF-DistributedInference/cpp/ndnsf-di/api.hpp',
        'NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp',
        'NDNSF-DistributedInference/cpp/ndnsf-di/provider.hpp',
        'NDNSF-DistributedInference/cpp/ndnsf-di/extensions.hpp',
    ]
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/ndnsf-di',
        [bld.path.find_resource(header) for header in di_installed_headers])
    # Stable short include names for external C++ applications. The nested
    # historical path remains installed for compatibility.
    bld.install_files(
        '${INCLUDEDIR}/ndnsf-di',
        [bld.path.find_resource('ndnsf-di/api.hpp'),
         bld.path.find_resource('ndnsf-di/provider.hpp')])
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/ndnsf-di',
        bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/ndnsf-di/*.hpp',
            excl=[
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp',
                'NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp',
            ]))
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/adapters/onnx',
        bld.path.ant_glob(
            'NDNSF-DistributedInference/cpp/adapters/onnx/*.hpp',
            excl=['NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp']))
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/adapters/yolo',
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/yolo/*.hpp'))
    bld.install_files(
        '${INCLUDEDIR}/NDNSF-DistributedInference/cpp/adapters/qwen',
        bld.path.ant_glob('NDNSF-DistributedInference/cpp/adapters/qwen/*.hpp'))

    # Export the same external header closure used by the native targets so a
    # standalone installed consumer can compile every public header without
    # inheriting this repository's build include paths.  Dependency paths are
    # generated from the explicitly selected configure-time prefixes; third-
    # party headers are marked system headers to keep consumer -Werror useful.
    package_include_flags = []
    package_include_paths = []
    # NDN-SVS may be supplied as an explicit source/build pair for the
    # development link closure.  Those paths are not an installed SDK and
    # must never leak into a relocatable consumer's pkg-config metadata;
    # installed consumers resolve NDN-SVS through their system package.
    for include_key in ('INCLUDES_NAC_ABE', 'INCLUDES_NAC-ABE'):
        for include_path in list(getattr(bld.env, include_key, []) or []):
            if include_path and include_path not in package_include_paths:
                package_include_paths.append(include_path)
                package_include_flags.extend(['-isystem', include_path])
    package_extra_includes = ' '.join(package_include_flags)

    bld(features='subst',
        source='NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in',
        target='ndnsf-distributed-inference.pc',
        install_path='${LIBDIR}/pkgconfig',
        VERSION=VERSION, EXTRA_INCLUDES=package_extra_includes)

    bld(features='subst',
        source='libndn-service-framework.pc.in',
        target='libndn-service-framework.pc',
        install_path='${LIBDIR}/pkgconfig',
        VERSION=VERSION, EXTRA_INCLUDES=package_extra_includes)

    # A selected-target install still needs the public header installation
    # tasks. Waf otherwise posts only the explicitly named native targets and
    # silently omits these anonymous install_files generators. Do not change
    # the target filter for compilation (especially the native test suites).
    if bld.cmd == 'install':
        for group in bld.groups:
            for task_generator in list(group):
                if 'install_task' in getattr(task_generator, 'features', []):
                    task_generator.post()
