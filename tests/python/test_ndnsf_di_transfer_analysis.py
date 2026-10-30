"""Check transfer analysis denominators, independent windows and missing evidence."""
import importlib.util
import json
from pathlib import Path


def load_analyzer():
    source = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di/analyze_transfer_measurement.py'
    spec = importlib.util.spec_from_file_location('transfer_analysis', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture(tmp_path, *, complete=True):
    evidence, capture = tmp_path / 'evidence', tmp_path / 'capture'
    evidence.mkdir()
    capture.mkdir()
    (capture / 'capture-start.json').write_text(json.dumps(dict(
        ready_epoch_ns=1000 * 10**9, ready_steady_ns=10 * 10**9)))
    request = dict(type='request', turn=0, requestId='/request/1',
                   startedSteadyNs=10 * 10**9, finishedSteadyNs=20 * 10**9,
                   startedProcessCpuNs=0, finishedProcessCpuNs=2 * 10**9)
    (evidence / 'requester-statistics.jsonl').write_text(json.dumps(request))
    producer = ('1000.5 WARN: NDNSF_DI_DEPENDENCY_OUTPUT_TIMING session=/request/1/attempt/1 '
                'planned_name=/producer/tensor bytes=64 output_ready_epoch_ms=1000500 '
                'publish_done_epoch_ms=1000600\n')
    received = ('1001.0 WARN: NDNSF_DI_DEPENDENCY_INPUT_TIMING session=/request/1/attempt/1 '
                'role=consumer producer=producer planned_name=/producer/tensor '
                'tensor_bytes=64 encoded_payload_bytes=80 transport_payload_bytes=90 '
                'wire_bytes=130 interest_count=1 retry_count=0 fetch_ms=700\n')
    if complete:
        received += ('1001.0 WARN: NDNSF_DI_PROVIDER_STAGE stage=DEPENDENCY_FETCH '
                     'status=complete requestId=/request/1 attemptEpoch=1 '
                     'plannedName=/producer/tensor steady_us=11000000\n')
    (evidence / 'provider-0.log').write_text(producer)
    (evidence / 'provider-1.log').write_text(received)
    timings = []
    for epoch, (start, end) in enumerate([(12, 17), (15, 19)]):
        timings.append(dict(inferenceEpoch=epoch, lastBackendExecution=dict(
            startedSteadyNs=start * 10**9, finishedSteadyNs=end * 10**9)))
    (evidence / 'provider-0-statistics.jsonl').write_text(json.dumps(dict(
        requestId='/request/1', provider='producer', role='producer', runtimeMetrics=timings)))
    (capture / 'link-packets.tsv').write_text(
        'frame.time_epoch\tframe.len\tframe.cap_len\tip.len\tudp.length\tip.src\tip.dst\tudp.srcport\tudp.dstport\n'
        '1001.0\t128\t96\t114\t94\t10.0.0.1\t10.0.0.2\t6363\t6363\n'
        '999.9\t60\t60\t46\t26\t10.0.0.1\t10.0.0.2\t6363\t6363\n')
    return evidence, capture


def test_counts_each_semantic_transfer_once_and_original_packet_lengths(tmp_path):
    result = load_analyzer().analyze(*fixture(tmp_path))
    row = result['turns'][0]
    assert row['admitted_objects'] == 1
    assert row['semantic_bytes']['encoded_payload_bytes'] == 80
    assert row['semantic_bytes']['signed_data_wire_bytes'] == 130
    assert row['link_frame_bytes'] == 128
    assert row['link_udp_payload_bytes'] == 86
    assert result['link_total']['frame_bytes'] == 188
    assert result['link_outside_request_loops']['frame_bytes'] == 60
    assert row['semantic_encoded_bytes_per_request_second'] == 8


def test_uses_union_for_overlapping_backend_and_clock_boundaries(tmp_path):
    row = load_analyzer().analyze(*fixture(tmp_path))['turns'][0]
    assert row['backend_union_seconds'] == 7
    assert row['backend_calls'] == 2
    assert row['ready_to_admitted_ms']['median'] == 500
    assert row['published_to_admitted_ms']['median'] == 400


def test_missing_admission_clock_remains_missing(tmp_path):
    result = load_analyzer().analyze(*fixture(tmp_path, complete=False))
    row = result['turns'][0]
    assert row['missing_complete_clocks'] == 1
    assert row['ready_to_admitted_ms']['count'] == 0
    assert row['ready_to_admitted_ms']['median'] is None
    assert result['objects'][0]['ready_to_admitted_ms'] is None


def test_fragment_without_udp_header_preserves_frame_bytes(tmp_path):
    evidence, capture = fixture(tmp_path)
    with (capture / 'link-packets.tsv').open('a') as stream:
        stream.write('1001.1\t1500\t96\t1486\t\t10.0.0.1\t10.0.0.2\t\t\n')
    result = load_analyzer().analyze(evidence, capture)
    row = result['turns'][0]
    assert row['link_frame_bytes'] == 1628
    assert row['link_missing_udp_headers'] == 1
    assert row['link_udp_payload_bytes'] is None


def test_missing_native_samples_are_not_measured_zero(tmp_path):
    evidence, capture = fixture(tmp_path)
    (evidence / 'provider-1.log').write_text('')
    (evidence / 'provider-0-statistics.jsonl').write_text('')
    row = load_analyzer().analyze(evidence, capture)['turns'][0]
    assert row['admitted_objects'] == 0
    assert row['semantic_bytes']['encoded_payload_bytes'] is None
    assert row['backend_union_seconds'] is None


def test_request_prefix_collision_does_not_cross_execution(tmp_path):
    evidence, capture = fixture(tmp_path)
    request = json.loads((evidence / 'requester-statistics.jsonl').read_text())
    request.update(requestId='/request/10', turn=1)
    with (evidence / 'requester-statistics.jsonl').open('a') as stream:
        stream.write('\n' + json.dumps(request))
    path = evidence / 'provider-1.log'
    path.write_text(path.read_text().replace('/request/1', '/request/10'))
    result = load_analyzer().analyze(evidence, capture)
    assert result['objects'][0]['request_id'] == '/request/10'
    assert result['objects'][0]['matched_producer_count'] == 0
    assert result['turns'][0]['admitted_objects'] == 0


def feedback_fixture(tmp_path, consumer='consumer'):
    evidence, capture = fixture(tmp_path)
    name = '/8=producer/8=ATTEMPT/8=2/8=feedback'
    path = evidence / 'provider-1.log'
    path.write_text(path.read_text().replace('/producer/tensor', name)
                    .replace('/request/1/attempt/1', '/request/1')
                    .replace('attemptEpoch=1', 'attemptEpoch=2'))
    (evidence / 'provider-0.log').write_text(
        '1000.6 WARN: NDNSF_DI_DEPENDENCY_OBJECT session=/request/1 '
        'producer=producer consumer=' + consumer + ' direction=publish-exact-ndn '
        'planned_name=' + name + ' encoded_payload_bytes=60\n')
    return evidence, capture


def test_feedback_publication_fallback_uses_canonical_attempt_and_own_clock(tmp_path):
    result = load_analyzer().analyze(*feedback_fixture(tmp_path))
    row = result['objects'][0]
    assert row['attempt_epoch'] == '2'
    assert row['matched_producer_count'] == 1
    assert row['packed_publication_payload_bytes'] == 60
    assert row['ready_to_admitted_ms'] is None
    assert row['publication_record_to_admitted_ms'] == 400


def test_feedback_publication_with_foreign_consumer_does_not_match(tmp_path):
    result = load_analyzer().analyze(*feedback_fixture(tmp_path, consumer='foreign'))
    row = result['objects'][0]
    assert row['matched_producer_count'] == 0
    assert row['publication_record_to_admitted_ms'] is None
