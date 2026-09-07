"""Shared real receipt validator; Worker wait/liveness calls are fixtures."""
import copy
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo


@pytest.fixture
def owner(monkeypatch):
    root = Path(__file__).resolve().parents[3]
    monkeypatch.setattr(yolo, 'APP_DIR', str(root / 'examples/python/NDNSF-DistributedInference/yolo_2x2'))
    return yolo._installed_yolo_owner()


def documents():
    common = dict(catalogueDataName='/controller/NDNSF/DI/catalogue/v1', catalogueSigner='/controller',
                  cataloguePayloadDigest='sha256:' + 'a' * 64,
                  artifacts=[{'dataName': '/controller/NDNSF/DI/ARTIFACT/one',
                              'payloadDigest': 'sha256:' + 'b' * 64}])
    expected = copy.deepcopy(common)
    receipt = dict(schema='spec180-runtime-publication-receipt-v1', **copy.deepcopy(common))
    return expected, receipt


@pytest.mark.parametrize('fault', ['none', 'duplicate', 'missing', 'changed', 'wrong-signer', 'malformed'])
def test_shared_receipt_validator_preserves_exact_artifact_set(owner, fault):
    expected, receipt = documents()
    if fault == 'duplicate':
        receipt['artifacts'] *= 2
    elif fault == 'missing':
        receipt['artifacts'] = []
    elif fault == 'changed':
        receipt['artifacts'][0]['payloadDigest'] = 'sha256:' + 'c' * 64
    elif fault == 'wrong-signer':
        receipt['catalogueSigner'] = '/other'
    elif fault == 'malformed':
        receipt['artifacts'].append('not-an-artifact')
    if fault == 'none':
        assert owner.validate_runtime_publication_receipt(expected, receipt) is receipt
    else:
        with pytest.raises(owner.RunnerError):
            owner.validate_runtime_publication_receipt(expected, receipt)


def test_marker_fences_receipt_read_and_preparation_is_rechecked(tmp_path, owner, monkeypatch):
    monkeypatch.setattr(yolo, '_installed_yolo_owner',
                        lambda: pytest.fail('host readiness must not import a SIF-only path'))
    public, output = tmp_path / 'public', tmp_path / 'output'
    public.mkdir()
    (output / 'controller').mkdir(parents=True)
    expected, receipt = documents()
    (public / 'runtime-publication.json').write_text(json.dumps(expected))
    events = []
    def marker(role, value, **kwargs):
        events.append('marker')
        assert value == 'SPEC180_RUNTIME_CATALOGUE_PUBLISHED'
        (output / 'controller/runtime-publication-receipt.json').write_text(json.dumps(receipt))
    worker = SimpleNamespace(rank=0, _preparation_binding=('pinned',), public=public, output=output,
        _verify_prepared_boundary=lambda: events.append('verify'), wait_marker=marker,
        check=lambda: events.append('alive'))
    assert yolo.wait_controller_publication(worker, seconds=1) == receipt
    assert events == ['verify', 'marker', 'verify', 'alive']


def test_native_ready_wait_uses_exact_bound_provider_identity():
    calls = []
    worker = SimpleNamespace(_preparation_binding=({'identities': {'Merge': '/actual/Merge'}},),
        roles=('Merge',), _verify_prepared_boundary=lambda: None, check=lambda: None,
        wait_marker=lambda *args, **kwargs: calls.append((args, kwargs)))
    yolo.wait_provider_ready(worker, 'Merge', seconds=2)
    assert calls[0][0] == ('Merge', 'NDNSF_DI_NATIVE_PROVIDER_READY provider=/actual/Merge activeRoles=1\n')
    assert calls[0][1]['seconds'] == 2
    with pytest.raises(ValueError):
        yolo.wait_provider_ready(worker, 'BackboneNeck', seconds=2)
