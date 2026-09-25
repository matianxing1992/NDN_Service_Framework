#!/usr/bin/env python3
"""Read-only inventory; Waf remains authoritative for ABI/compile/link checks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def check_dependencies(nac_prefix='/usr/local', onnx_prefix='/usr/local', sdk_only=False):
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
                'libnac-abe', 'ndnsd', 'openssl', 'protobuf', 'gtkmm-3.0', 'sqlite3',
                'onnxruntime >= 1.26.0')
    if sdk_only:
        required = ('onnxruntime >= 1.26.0',)
    if not pkg:
        missing.append('tool: pkg-config/pkgconf (dependency metadata not checked)')
    else:
        for name in required:
            if subprocess.run([pkg, '--exists', name], env=env).returncode:
                missing.append('pkg-config: ' + name)
        gst = 'gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0'
        if not sdk_only and subprocess.run([pkg, '--exists', *gst.split()], env=env).returncode:
            optional.append('Optional GStreamer development packages missing; video backend disabled')
    files = [Path(nac_prefix) / 'include/nac-abe/consumer.hpp',
             Path(nac_prefix) / 'lib/libnac-abe.so']
    if sdk_only:
        files = []
    files += [Path(onnx_prefix) / suffix for suffix in (
        'include/onnx/checker.h', 'include/onnx/shape_inference/implementation.h',
        'lib/libonnx.a', 'lib/libonnx_proto.a')]
    for path in files:
        if not path.is_file():
            missing.append('installed SDK file: ' + str(path))
    archives = [os.environ.get('NDNSF_TOKENIZER_BRIDGE_ARCHIVE', ''),
                '/usr/local/lib/libndnsf_tokenizer_bridge.a',
                '/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a']
    if not any(path and Path(path).is_file() for path in archives):
        missing.append('installed SDK: libndnsf_tokenizer_bridge.a')
    return missing, optional


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nac-abe-prefix', default='/usr/local')
    parser.add_argument('--onnx-prefix', default='/usr/local')
    parser.add_argument('--sdk-only', action='store_true',
                        help='Check preinstalled ONNX/ORT/tokenizer SDKs only')
    args = parser.parse_args()
    errors, warnings = check_dependencies(args.nac_abe_prefix, args.onnx_prefix, args.sdk_only)
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
