# -*- Mode: python; py-indent-offset: 4; indent-tabs-mode: nil; coding: utf-8; -*-

from waflib import Context, Logs, Utils
import os, subprocess

VERSION = '0.1.0'
APPNAME = 'ndn-service-framework'
GIT_TAG_PREFIX = 'ndn-service-framework-'

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

    conf.load(['compiler_cxx', 'gnu_dirs',
               'default-compiler-flags', 'boost',
               'doxygen'])

    conf.env.WITH_EXAMPLES = conf.options.with_examples
    conf.env.WITH_TESTS = conf.options.with_tests

    conf.find_program('dot', mandatory=False)

    # Prefer pkgconf if it's installed, because it gives more correct results
    # on Fedora/CentOS/RHEL/etc. See https://bugzilla.redhat.com/show_bug.cgi?id=1953348
    # Store the result in env.PKGCONFIG, which is the variable used inside check_cfg()
    conf.find_program(['pkgconf', 'pkg-config'], var='PKGCONFIG')

    pkg_config_path = os.environ.get('PKG_CONFIG_PATH', f'{conf.env.LIBDIR}/pkgconfig')
    conf.check_cfg(package='libndn-cxx', args=['libndn-cxx >= 0.8.0', '--cflags', '--libs'],
                   uselib_store='NDN_CXX', pkg_config_path=pkg_config_path)

    
    conf.check_cfg(package='libndn-svs', args=['libndn-svs >= 0.1.0', '--cflags', '--libs'],
                       uselib_store='NDN_SVS', pkg_config_path=pkg_config_path)

    conf.check(features='cxx cxxprogram', lib=['sqlite3'], cflags=['-Wall'], defines=['var=foo'], uselib_store='sqlite3')


    conf.check_cfg(package='libnac-abe', args=['--cflags', '--libs'], uselib_store='NAC-ABE',
                   pkg_config_path=pkg_config_path)
    
    conf.check_cfg(package='ndnsd', args=['--cflags', '--libs'], uselib_store='NDNSD',
                   pkg_config_path=pkg_config_path)

    # protobuf
    conf.check_cfg(package="protobuf", uselib_store="PROTOBUF", 
            args=['--cflags', '--libs'])
    conf.find_program('protoc', var='PROTOC')

    # MAVSDK libmavsdk-dev_1.4.16
    conf.check_cfg(package="mavsdk", uselib_store="MAVSDK", 
            args=['--cflags', '--libs'])
    
    # gtkmm-3.0
    conf.check_cfg(package="gtkmm-3.0", uselib_store="gtkmm", 
            args=['--cflags', '--libs'], pkg_config_path=pkg_config_path)

    boost_libs = ['system']
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
    # The config header will contain all defines that were added using conf.define()
    # or conf.define_cond().  Everything that was added directly to conf.env.DEFINES
    # will not appear in the config header, but will instead be passed directly to the
    # compiler on the command line.
    conf.write_config_header('config.hpp')

def build(bld):
    libndn_service_framework = dict(
        target='ndn-service-framework',
        vnum=VERSION,
        cnum=VERSION,
        source=bld.path.ant_glob('ndn-service-framework/**/*.cpp'),
        use='NDN_CXX NDN_SVS BOOST PROTOBUF NAC-ABE',
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

    if bld.env.WITH_EXAMPLES:
        bld.recurse('examples')

    headers = bld.path.ant_glob('ndn-service-framework/**/*.hpp')
    bld.install_files('${INCLUDEDIR}', headers, relative_trick=True)

    bld.install_files('${INCLUDEDIR}/ndn-service-framework',
                      bld.path.find_resource('config.hpp'))

    bld(features='subst',
        source='libndn-service-framework.pc.in',
        target='libndn-service-framework.pc',
        install_path='${LIBDIR}/pkgconfig',
        VERSION=VERSION)

