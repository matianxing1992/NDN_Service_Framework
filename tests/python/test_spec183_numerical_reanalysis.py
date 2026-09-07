"""Actual producer function, tensor codec and NumPy oracle; no native/model run."""
import hashlib
import json
from pathlib import Path
import sys
import subprocess
from types import SimpleNamespace

import pytest

from test_spec180_yolo_numerical import reference, _production_functions, ROOT, load_reference

sys.path.insert(0, str(ROOT / 'Experiments/TigerCluster'))
from runtime.yolo_result import reanalyze_numerical_response


@pytest.mark.parametrize('fault', ['none', 'plan', 'response', 'catalogue'])
def test_request_collection_joins_actual_numerical_bytes_to_lifecycle(reference, tmp_path, monkeypatch, fault):
    from runtime.yolo_result import collect_request_result
    sys.path.insert(0, str(ROOT / 'Experiments/TigerCluster/tests'))
    from test_yolo_lifecycle_result import journal, D
    package, expected = reference
    frozen = load_reference(package, ROOT, 640)
    functions = _production_functions()
    payload = functions['encode_native_tensor_bundle']({'predictions': expected})
    monkeypatch.setenv('SPEC180_CANDIDATE_ID', 'shared-backbone-two-shard-v1')
    monkeypatch.setenv('SPEC180_CANDIDATE_DIGEST', D)
    args = SimpleNamespace(lifecycle_output_dir=str(tmp_path), lifecycle_case='two-node',
                           request_id='/run/request/1', retain_numerical_response=True)
    functions['_record_yolo_numerical_result'](args, frozen, payload, D, 'attempt-1')
    rows = journal()
    rows[-1]['resultDigest'] = 'sha256:' + hashlib.sha256(payload).hexdigest()
    if fault == 'plan': rows[6]['planDigest'] = 'sha256:'+'2'*64
    if fault == 'response': rows[-1]['resultDigest'] = D
    if fault == 'catalogue': rows[3]['catalogueDigest'] = 'sha256:'+'2'*64
    (tmp_path/'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
    kwargs = dict(case='two-node', request_id='/run/request/1', attempt_id='attempt-1',
        candidate_id='shared-backbone-two-shard-v1', candidate_digest=D, graph_digest=D, catalogue_digest=D)
    if fault == 'none':
        assert collect_request_result(tmp_path, frozen, **kwargs)['qualification'] == 'REQUEST_RESULT_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError):
            collect_request_result(tmp_path, frozen, **kwargs)


@pytest.mark.parametrize('fault', ['none', 'forged-pass', 'payload', 'lineage', 'oracle', 'tolerance', 'path', 'symlink'])
def test_reanalysis_uses_bytes_not_pass_flag(reference, tmp_path, monkeypatch, fault):
    package, expected = reference
    functions = _production_functions()
    frozen = load_reference(package, ROOT, 640)
    if fault == 'forged-pass':
        expected[0, 0, 0] += 0.1
    payload = functions['encode_native_tensor_bundle']({'predictions': expected})
    monkeypatch.setenv('SPEC180_CANDIDATE_ID', 'candidate')
    monkeypatch.setenv('SPEC180_CANDIDATE_DIGEST', 'sha256:' + 'b' * 64)
    args = SimpleNamespace(lifecycle_output_dir=str(tmp_path), lifecycle_case='local-cpu',
                           request_id='/app/request', retain_numerical_response=True)
    functions['_record_yolo_numerical_result'](args, frozen, payload, 'sha256:' + 'a' * 64, 'attempt-1')
    record_path, binary = tmp_path / 'yolo-numerical.json', tmp_path / 'yolo-response.bin'
    record = json.loads(record_path.read_text())
    assert binary.read_bytes() == payload
    assert binary.stat().st_mode & 0o777 == 0o600
    if fault == 'forged-pass':
        record['matched'] = True
    elif fault == 'payload':
        binary.write_bytes(payload[:-1])
    elif fault == 'lineage':
        record['attemptId'] = 'old-attempt'
    elif fault == 'oracle':
        record['oracleDigest'] = 'sha256:' + 'c' * 64
    elif fault == 'tolerance':
        record['atol'] = 100
    elif fault == 'path':
        record['responsePath'] = '../foreign.bin'
    elif fault == 'symlink':
        binary.rename(tmp_path / 'original.bin')
        binary.symlink_to(tmp_path / 'original.bin')
    record_path.write_text(json.dumps(record))
    kwargs = dict(case='local-cpu', request_id='/app/request', attempt_id='attempt-1',
        plan_digest='sha256:' + 'a' * 64, result_digest='sha256:' + hashlib.sha256(payload).hexdigest(),
        candidate_id='candidate', candidate_digest='sha256:' + 'b' * 64)
    if fault == 'none':
        result = reanalyze_numerical_response(tmp_path, frozen, **kwargs)
        assert result['matched'] is True and result['qualification'] == 'NUMERICAL_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError):
            reanalyze_numerical_response(tmp_path, frozen, **kwargs)
    # Freshness is not relaxed even when the old comparison failed.
    with pytest.raises((ValueError, FileExistsError)):
        functions['_record_yolo_numerical_result'](args, frozen, payload, 'sha256:' + 'a' * 64, 'attempt-1')


@pytest.mark.parametrize('retained', [False, True])
def test_response_retention_is_bounded_and_opt_in(reference, tmp_path, retained):
    package, _ = reference
    functions = _production_functions()
    args = SimpleNamespace(lifecycle_output_dir=str(tmp_path), lifecycle_case='local-cpu',
                           request_id='/app/request', retain_numerical_response=retained)
    if retained:
        with pytest.raises(ValueError, match='RETENTION_SIZE'):
            functions['_record_yolo_numerical_result'](args, load_reference(package, ROOT, 640),
                                                      b'x' * (1024 * 1024 + 1), 'plan', 'attempt')
        assert not (tmp_path / 'yolo-response.bin').exists()
        assert not (tmp_path / 'yolo-numerical.json').exists()
    else:
        functions['_record_yolo_numerical_result'](args, load_reference(package, ROOT, 640),
                                                  b'invalid', 'plan', 'attempt')
        assert not (tmp_path / 'yolo-response.bin').exists()


def test_oracle_and_decoder_import_without_native_extension_in_fresh_process():
    code = ('import sys; sys.path.insert(0, sys.argv[1]); '
            'from ndnsf_distributed_inference.adapters.yolo.reference import compare_reference; '
            'from ndnsf_distributed_inference.adapters.yolo.tensor_bundle import decode_tensor_bundle; '
            'assert "ndnsf" not in sys.modules; '
            'assert not any(k.endswith("._ndnsf") for k in sys.modules)')
    subprocess.run([sys.executable, '-c', code, str(ROOT / 'NDNSF-DistributedInference')],
                   check=True, timeout=10, capture_output=True, text=True)
