"""Typed prerequisite boundaries; collector doubles do not prove runtime PASS."""
import copy
import json

import pytest

from test_yolo_submit import file_ref, submit_module


@pytest.fixture
def gate_pair(tmp_path):
    from tools.spec183_dispatch_plane import _sealed_harness
    module = submit_module()
    (tmp_path / 'sealed').mkdir()
    seal = _sealed_harness(tmp_path / 'sealed')
    bundle = tmp_path / 'sealed/harness'
    values = []
    for run, case, char in [('previous-run', 'local-cpu', 'a'),
                            ('current-run', 'single-node-gpu', 'b')]:
        root = tmp_path / run
        root.mkdir()
        digest = 'sha256:' + char * 64
        plan = dict(runId=run, case=case, output=str(root), documentDigest=digest,
                    effectiveBehavior={'profile': {'same': 'normalized behavior'}})
        value = dict(schema='tiger-yolo-prepared-run-v2', status='PREPARED',
            qualification='NOT_EVALUATED', runId=run, case=case, profileDigest=digest,
            plan=plan, bundle=str(bundle), harnessManifestSha256=seal['manifestSha256'],
            contentIdentities=dict.fromkeys(('inputs', 'runtime', 'dispatch'), 'sha256:'+'c'*64))
        value['candidateDigest'] = module._prepared_candidate(value)
        (root / 'prepare.json').write_text(json.dumps(value))
        values.append(value)
    previous, current = values
    verdict = dict(status='PASS', qualification='NORMAL_EXPERIMENT_PASS', case='local-cpu',
        collectorSchema='tiger-yolo-collector-v1', runId=previous['runId'],
        candidateDigest=previous['candidateDigest'])
    path = tmp_path / previous['runId'] / 'verdict.json'
    path.write_text(json.dumps(verdict))
    profile = {'release': {'gates': {'localSif': file_ref(path)}}}
    return module, previous, current, verdict, profile, tmp_path


def test_prior_profile_can_differ_but_retained_evidence_must_be_reanalyzed(gate_pair, monkeypatch):
    module, previous, current, verdict, profile, root = gate_pair
    calls = []
    def reanalyze(path, saved):
        calls.append((path, saved))
        return verdict
    monkeypatch.setattr(module, '_reanalyze_retained', reanalyze)
    before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}
    result = module._gate_receipt(root / 'profile.json', profile, 'localSif', prepared=current)
    assert result['receipt'] == verdict
    assert calls == [(root / previous['runId'], previous)]
    assert before == {p: p.read_bytes() for p in root.rglob('*') if p.is_file()}


@pytest.mark.parametrize('fault', ['runtime', 'dispatch', 'inputs', 'behavior', 'same-run',
                                  'harness', 'reanalysis', 'missing-evidence', 'generic-pass'])
def test_prior_gate_rejects_incompatible_or_unproven_evidence(gate_pair, monkeypatch, fault):
    module, previous, current, verdict, profile, root = gate_pair
    current = copy.deepcopy(current)
    if fault in ('runtime', 'dispatch', 'inputs'):
        current['contentIdentities'][fault] = 'sha256:' + 'd'*64
    elif fault == 'behavior':
        current['plan']['effectiveBehavior']['profile']['same'] = 'changed'
    elif fault == 'same-run':
        current['runId'] = previous['runId']
    elif fault == 'harness':
        current['harnessManifestSha256'] = 'sha256:' + 'd'*64
    elif fault == 'generic-pass':
        path = root / previous['runId'] / 'verdict.json'
        path.write_text(json.dumps(dict(status='PASS', qualification='PASS')))
        profile['release']['gates']['localSif'] = file_ref(path)
    def reanalyze(*args):
        if fault == 'missing-evidence':
            raise FileNotFoundError('retained collection missing')
        return dict(verdict, status='FAIL') if fault == 'reanalysis' else verdict
    monkeypatch.setattr(module, '_reanalyze_retained', reanalyze)
    with pytest.raises(module.ClosureError):
        module._gate_receipt(root / 'profile.json', profile, 'localSif', prepared=current)


@pytest.mark.parametrize('fault', ['legacy', 'plan', 'content', 'candidate-type', 'profile-type', 'harness-type'])
def test_prepared_receipt_rejects_stale_or_mutated_binding(gate_pair, fault):
    module, previous, _, _, _, root = gate_pair
    assert module._load_prepared(root, previous['runId']) == previous
    if fault == 'legacy':
        previous['schema'] = 'tiger-yolo-prepared-run-v1'
    elif fault == 'plan':
        previous['plan']['output'] = str(root / 'elsewhere')
    elif fault == 'content':
        previous['contentIdentities']['runtime'] = 'sha256:'+'d'*64
    else:
        key = {'candidate-type': 'candidateDigest', 'profile-type': 'profileDigest',
               'harness-type': 'harnessManifestSha256'}[fault]
        previous[key] = 1
    (root / previous['runId'] / 'prepare.json').write_text(json.dumps(previous))
    with pytest.raises(module.ClosureError):
        module._load_prepared(root, previous['runId'])
