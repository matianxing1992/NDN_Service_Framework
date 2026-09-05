"""spec181 R004: protected-epoch execution fails closed before grant wiring.

Until T001/T002 wire real grant verification, an execution request that
declares a non-plaintext protection epoch (the Y-B protected-epoch subcase
regime) must be refused with DI_PROTECTED_GRANT_UNAVAILABLE before any
output root or MiniNDN side effect.  Plaintext-v1 requests and requests
without the epoch declaration are unaffected.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec181_runner_guard", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def module_env():
    module = load_runner()
    env = dict(module.os.environ)
    env.pop(module.PROTECTION_EPOCH_ENV, None)
    return module, env


def test_guard_exists_and_allows_no_epoch_declaration(module_env):
    module, env = module_env
    module._assert_grant_wiring_or_plaintext(env)


def test_guard_allows_explicit_plaintext_epoch(module_env):
    module, env = module_env
    env[module.PROTECTION_EPOCH_ENV] = module.PLAINTEXT_EPOCH
    module._assert_grant_wiring_or_plaintext(env)


def test_guard_allows_protected_epoch_after_wiring(module_env):
    # T001/T002 landed: the protected epoch proceeds to the real verifier
    # (GRANT_WIRING_AVAILABLE is True on this branch).
    module, env = module_env
    env[module.PROTECTION_EPOCH_ENV] = "spec180-yolo-protected-v1"
    module._assert_grant_wiring_or_plaintext(env)


def test_guard_unknown_epoch_proceeds_after_absorption(module_env):
    # T001/T002 absorbed the R004 gate: GRANT_WIRING_AVAILABLE is True, so
    # any non-plaintext epoch (including an unsealed future value) proceeds
    # to the real verifier instead of failing closed.
    module, env = module_env
    env[module.PROTECTION_EPOCH_ENV] = "epoch-yet-unsealed"
    module._assert_grant_wiring_or_plaintext(env)


def test_guard_absorption_flip_allows_protected_execution(module_env):
    # T001/T002 absorption flips GRANT_WIRING_AVAILABLE; the protected
    # request then proceeds to the real verifier instead of failing closed.
    module, env = module_env
    env[module.PROTECTION_EPOCH_ENV] = "spec180-yolo-protected-v1"
    module.GRANT_WIRING_AVAILABLE = True
    module._assert_grant_wiring_or_plaintext(env)


def test_main_protected_epoch_reaches_input_validation_after_absorption(
        monkeypatch, capsys):
    module = load_runner()
    env = dict(module.os.environ)
    env[module.PROTECTION_EPOCH_ENV] = "spec180-yolo-protected-v1"
    monkeypatch.setattr(module.os, "environ", env)
    reached = []

    def validate_inputs_reached(*_args, **_kwargs):
        reached.append(True)
        # The real input closure is exercised by the matrix; the unit seam
        # only proves the guard no longer fires before validation.
        raise module.RunnerError("SIMULATED_INPUT_WAIT")

    monkeypatch.setattr(module, "validate_inputs", validate_inputs_reached)
    assert module.main(["--case", "Y-B"]) == 78
    assert reached
    out = capsys.readouterr().out
    assert "status=WAITING_EXTERNAL_INPUT" in out


def test_main_plaintext_case_still_reaches_input_validation(monkeypatch, capsys):
    module = load_runner()
    env = dict(module.os.environ)
    env.pop(module.PROTECTION_EPOCH_ENV, None)
    monkeypatch.setattr(module.os, "environ", env)
    reached = []

    def fake_validate_inputs(case, environment):
        reached.append(case)
        raise module.RunnerError("INPUT_TEST_STOP")

    monkeypatch.setattr(module, "validate_inputs", fake_validate_inputs)
    assert module.main(["--case", "Y-A"]) == 78
    assert reached == ["Y-A"]
