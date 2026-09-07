"""Paired source-format log fixtures, not real transport."""
from pathlib import Path
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_result import validate_dependency_edges


@pytest.mark.parametrize('fault', ['none','missing','bytes','name','session','duplicate','transport','owner'])
def test_dependency_requires_both_bound_data_v1_directions(tmp_path, fault):
    edge = dict(scope='backbone-to-head0', producer='BackboneNeck', consumer='DetectShard0', planned_name='/app/data/1')
    logs = {}
    for role, direction in [('BackboneNeck','publish'), ('DetectShard0','fetch')]:
        row = dict(session='/request/1', **edge, direction=direction+'-ndnsf-data-v1', payload_bytes='123', status='ok')
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
