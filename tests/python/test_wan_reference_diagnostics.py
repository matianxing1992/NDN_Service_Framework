"""Host diagnostic guards only; fake modules do not qualify tensor arithmetic or CUDA."""

import importlib.util
from pathlib import Path
import sys
import types

import pytest


@pytest.fixture
def diagnostics(monkeypatch):
    torch = types.ModuleType('torch')
    torch.float32 = 'fixture-float32'
    torch.nn = types.SimpleNamespace(functional=types.SimpleNamespace(linear=object()))
    torch.matmul = lambda *args: pytest.fail('guard unexpectedly reached tensor arithmetic')
    tokenizers = types.ModuleType('tokenizers')
    tokenizers.Tokenizer = object
    for name, module in [('torch', torch), ('numpy', types.ModuleType('numpy')),
                         ('tokenizers', tokenizers)]:
        monkeypatch.setitem(sys.modules, name, module)
    path = Path(__file__).resolve().parents[2] / 'Experiments/prepare-wan22-video-reference.py'
    spec = importlib.util.spec_from_file_location('wan_reference_diagnostics', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, torch


@pytest.mark.parametrize('flag', ['--dit-fp64-diagnostic', '--dit-unfused-linear-diagnostic'])
@pytest.mark.parametrize('device,clip', [('cpu', True), ('cuda', False)])
def test_arithmetic_diagnostic_rejects_missing_cuda_or_actual_inputs(
        diagnostics, monkeypatch, flag, device, clip):
    module, _ = diagnostics
    args = ['reference', '--asset-root', 'unused', '--workload', 'unused',
            '--initial-latent', 'unused', '--initial-receipt', 'unused',
            '--output', 'unused', '--device', device, flag]
    if clip:
        args += ['--native-clip', 'unused']
    monkeypatch.setattr(sys, 'argv', args)
    with pytest.raises(SystemExit) as error:
        module.main()
    assert error.value.code == 2


def test_diagnostic_modes_cannot_be_combined(diagnostics, monkeypatch):
    module, _ = diagnostics
    monkeypatch.setattr(sys, 'argv', [
        'reference', '--asset-root', 'unused', '--workload', 'unused',
        '--initial-latent', 'unused', '--initial-receipt', 'unused',
        '--output', 'unused', '--device', 'cuda', '--native-clip', 'unused',
        '--dit-fp64-diagnostic', '--dit-unfused-linear-diagnostic'])
    with pytest.raises(SystemExit) as error:
        module.main()
    assert error.value.code == 2


@pytest.mark.parametrize('flag', [None, '--dit-fp64-diagnostic',
                                '--dit-unfused-linear-diagnostic'])
def test_valid_cli_modes_reach_device_setup_before_loading_files(
        diagnostics, monkeypatch, tmp_path, flag):
    module, torch = diagnostics

    def stop_before_execution(*args):
        raise RuntimeError('fixture stops before device setup or model loading')

    torch.device = stop_before_execution
    args = ['reference', '--asset-root', 'unused', '--workload', 'unused',
            '--initial-latent', 'unused', '--initial-receipt', 'unused',
            '--output', str(tmp_path / 'output'), '--device', 'cuda',
            '--native-clip', 'unused']
    if flag:
        args.append(flag)
    monkeypatch.setattr(sys, 'argv', args)
    with pytest.raises(RuntimeError, match='fixture stops before device setup'):
        module.main()


def test_unfused_diagnostic_restores_linear_after_arithmetic_failure(diagnostics):
    module, torch = diagnostics
    original = torch.nn.functional.linear

    def fail(*args):
        raise RuntimeError('fixture arithmetic failure')

    torch.matmul = fail
    tensor = types.SimpleNamespace(dtype=torch.float32, transpose=lambda *args: object())
    with pytest.raises(RuntimeError, match='fixture arithmetic failure'):
        with module.unfused_dit_diagnostic():
            torch.nn.functional.linear(tensor, tensor)
    assert torch.nn.functional.linear is original


def test_unfused_diagnostic_rejects_lower_precision_before_arithmetic(diagnostics):
    module, torch = diagnostics
    original = torch.nn.functional.linear
    tensor = types.SimpleNamespace(dtype='fixture-float16')
    with pytest.raises(ValueError, match='requires FP32'):
        with module.unfused_dit_diagnostic():
            torch.nn.functional.linear(tensor, tensor)
    assert torch.nn.functional.linear is original
