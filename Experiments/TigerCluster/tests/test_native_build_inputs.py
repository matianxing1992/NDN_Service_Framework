import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / 'adapters/slurm-apptainer/scripts/prepare-native-build-inputs.py'
spec = importlib.util.spec_from_file_location('native_build_inputs', SCRIPT)
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


@pytest.fixture
def inputs(tmp_path, monkeypatch):
    args = SimpleNamespace(**{name: tmp_path / name for name in
                             ['onnx_source', 'rust_archives', 'rust_prefix', 'cargo_home', 'crate', 'output']})
    for name, path in vars(args).items():
        if name != 'output': path.mkdir()
    (args.crate / 'Cargo.toml').write_text('locked test fixture')
    (args.crate / 'Cargo.lock').write_text('locked crate dependencies')
    (args.rust_archives / 'installer.tar.xz').write_bytes(b'unit installer')
    monkeypatch.setattr(native, 'RUST', {'installer.tar.xz': hashlib.sha256(b'unit installer').hexdigest()})
    monkeypatch.setattr(native.subprocess, 'check_output', lambda *a, **k: native.ONNX_REVISION)
    return args


def test_identity_rejected_before_output(inputs, monkeypatch):
    monkeypatch.setattr(native.subprocess, 'check_output', lambda *a, **k: 'wrong revision')
    with pytest.raises(ValueError, match='ONNX_REVISION'):
        native.prepare(inputs)
    assert not inputs.output.exists()


def test_toolchain_digest_rejected_before_output(inputs):
    (inputs.rust_archives / 'installer.tar.xz').write_bytes(b'changed installer')
    with pytest.raises(ValueError, match='RUST_DIST_DIGEST'):
        native.prepare(inputs)
    assert not inputs.output.exists()


def fake_run(args, **kwargs):
    if 'archive' in args:
        kwargs['stdout'].write(b'unit source archive')
    else:
        assert 'vendor' in args and '--locked' in args and '--offline' in args
        vendor = Path(args[-1])
        vendor.mkdir()
        (vendor / 'lib.rs').write_text('pub fn unit() {}')


def test_offline_sources_and_failure_record(inputs, monkeypatch):
    monkeypatch.setattr(native.subprocess, 'run', fake_run)
    record = native.prepare(inputs)
    assert record['crate']['Cargo.lock'] == native.digest(inputs.crate / 'Cargo.lock')
    assert json.loads((inputs.output / 'prepare-record.json').read_text())['status'] == 'SOURCE_READY'
    for name, expected in record['files'].items():
        assert native.digest(inputs.output / name) == expected


@pytest.mark.parametrize('failure', ['cargo', 'symlink'])
def test_failed_materialization_cannot_look_ready(inputs, monkeypatch, failure):
    def run(args, **kwargs):
        if 'vendor' in args and failure == 'cargo':
            raise subprocess.CalledProcessError(1, args)
        fake_run(args, **kwargs)
        if 'vendor' in args:
            (Path(args[-1]) / 'escaped').symlink_to(inputs.crate / 'Cargo.toml')
    monkeypatch.setattr(native.subprocess, 'run', run)
    with pytest.raises((ValueError, subprocess.CalledProcessError)):
        native.prepare(inputs)
    assert not (inputs.output / 'native-inputs.json').exists()
    assert json.loads((inputs.output / 'prepare-record.json').read_text())['status'] == 'FAIL'
