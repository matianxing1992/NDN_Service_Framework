"""Paired source-format log fixtures, not real transport."""
from pathlib import Path
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_result import validate_dependency_edges


@pytest.mark.parametrize('fault', ['none','missing','bytes','name','session','duplicate','transport','owner'])
@pytest.mark.parametrize('wire_direction', [False, True])
def test_dependency_requires_both_bound_data_v1_directions(tmp_path, fault, wire_direction):
    edge = dict(scope='backbone-to-head0', producer='BackboneNeck', consumer='DetectShard0', planned_name='/app/data/1')
    logs = {}
    for role, direction in [('BackboneNeck','publish'), ('DetectShard0','fetch')]:
        suffix = 'exact-ndn' if wire_direction else 'ndnsf-data-v1'
        row = dict(session='/request/1', **edge, direction=direction+'-'+suffix, payload_bytes='123', status='ok')
        if role == 'DetectShard0':
            if fault == 'bytes': row['payload_bytes'] = '124'
            if fault == 'name': row['planned_name'] = '/other/data'
            if fault == 'session': row['session'] = '/old'
            if fault == 'transport': row['direction'] = 'fetch'
            if fault == 'owner': row['consumer'] = 'Merge'
        line = '123 WARN: NDNSF_DI_DEPENDENCY_OBJECT ' + ' '.join(k+'='+v for k,v in row.items())+'\n'
        if role == 'DetectShard0' and fault == 'missing': line = ''
        if role == 'DetectShard0' and fault == 'duplicate': line *= 2
        logs[role] = tmp_path/(role+'.log')
        logs[role].write_text(line)
    if fault == 'none':
        assert validate_dependency_edges(logs, [edge], session_id='/request/1')['edgeCount'] == 1
    else:
        with pytest.raises(ValueError):
            validate_dependency_edges(logs, [edge], session_id='/request/1')


def test_dependency_accepts_native_leading_slash_session(tmp_path):
    edge = dict(scope='backbone-to-head0', producer='BackboneNeck',
                consumer='DetectShard0', planned_name='/app/data/1')
    logs = {}
    for role, direction in [('BackboneNeck', 'publish-exact-ndn'),
                            ('DetectShard0', 'fetch-exact-ndn')]:
        row = dict(session='/request/1', **edge, direction=direction,
                   payload_bytes='123', status='ok')
        path = tmp_path / (role + '.log')
        path.write_text('NDNSF_DI_DEPENDENCY_OBJECT ' +
                        ' '.join(k + '=' + v for k, v in row.items()) + '\n')
        logs[role] = path
    assert validate_dependency_edges(logs, [edge], session_id='request/1')['edgeCount'] == 1


def test_dependency_accepts_native_request_session_without_attempt_suffix(tmp_path):
    edge = dict(scope='backbone-to-head0', producer='BackboneNeck',
                consumer='DetectShard0', planned_name='/app/data/1')
    logs = {}
    for role, direction in [('BackboneNeck', 'publish-exact-ndn'),
                            ('DetectShard0', 'fetch-exact-ndn')]:
        row = dict(session='/request/1', **edge, direction=direction,
                   payload_bytes='123', status='ok')
        path = tmp_path / (role + '.log')
        path.write_text('NDNSF_DI_DEPENDENCY_OBJECT ' +
                        ' '.join(k + '=' + v for k, v in row.items()) + '\n')
        logs[role] = path
    assert validate_dependency_edges(
        logs, [edge], session_id='request/1/attempt/1')['edgeCount'] == 1


def test_dependency_matches_python_decimal_name_to_ndn_cxx_nni_name(tmp_path):
    edge = dict(
        scope='backbone-to-head0', producer='BackboneNeck',
        consumer='DetectShard0',
        planned_name='/x/ATTEMPT/1/ROUND/0/RANK/2/MICROBATCH/256')
    native_name = '/x/ATTEMPT/%01/ROUND/%00/RANK/%02/MICROBATCH/%01%00'
    logs = {}
    for role, direction in [('BackboneNeck', 'publish-exact-ndn'),
                            ('DetectShard0', 'fetch-exact-ndn')]:
        row = dict(edge, session='/request/1', planned_name=native_name,
                   direction=direction, payload_bytes='123', status='ok')
        path = tmp_path / (role + '.log')
        path.write_text('NDNSF_DI_DEPENDENCY_OBJECT ' +
                        ' '.join(k + '=' + v for k, v in row.items()) + '\n')
        logs[role] = path
    assert validate_dependency_edges(
        logs, [edge], session_id='/request/1')['edgeCount'] == 1
