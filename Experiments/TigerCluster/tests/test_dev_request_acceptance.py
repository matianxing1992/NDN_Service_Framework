"""The development runner must consume real output through production validators."""
from unittest.mock import Mock

import pytest

from runtime import yolo_graph_reference, yolo_result
from tools.spec183_dev_provision import validate_local_request


@pytest.mark.parametrize('failure', [None, 'graph', 'result'])
def test_acceptance_validates_and_preserves_request_directory(tmp_path, monkeypatch, failure):
    output = tmp_path / 'request-0'
    output.mkdir()
    (output / 'retained.json').write_text('{"original":true}')
    before = {p.name: p.read_bytes() for p in output.iterdir()}
    graph, result = Mock(), Mock(return_value={'validated': True})
    if failure == 'graph':
        graph.side_effect = ValueError('bad graph evidence')
    elif failure == 'result':
        result.side_effect = ValueError('bad result evidence')
    monkeypatch.setattr(yolo_graph_reference, 'read_request_reference', graph)
    monkeypatch.setattr(yolo_result, 'collect_request_result', result)
    prepared = dict(runId='run-1', candidateDigest='sha256:'+'a'*64)
    receipt = dict(placementCandidateDigest='sha256:'+'b'*64,
                   placementCandidateId='candidate-1', graphDigest='sha256:'+'c'*64,
                   catalogueDigest='sha256:'+'d'*64)
    reference = object()
    args = dict(reference=reference, prepared=prepared, receipt=receipt)
    if failure:
        with pytest.raises(ValueError, match='bad .* evidence'):
            validate_local_request({'requestId': '/request/1'}, output, **args)
    else:
        assert validate_local_request({'requestId': '/request/1'}, output, **args) == {'validated': True}
    graph.assert_called_once_with(output / 'graph-reference.json', run_id='run-1',
        request_id='/request/1', runtime_candidate_digest=prepared['candidateDigest'],
        placement_candidate_digest=receipt['placementCandidateDigest'], graph_digest=receipt['graphDigest'])
    if failure == 'graph':
        result.assert_not_called()
    else:
        result.assert_called_once_with(output, reference, case='local-cpu', request_id='/request/1',
            attempt_id='attempt-1', candidate_id='candidate-1',
            candidate_digest=receipt['placementCandidateDigest'], graph_digest=receipt['graphDigest'],
            catalogue_digest=receipt['catalogueDigest'])
    assert output.is_dir()
    assert {p.name: p.read_bytes() for p in output.iterdir()} == before
