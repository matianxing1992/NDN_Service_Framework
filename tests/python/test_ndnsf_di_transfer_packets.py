"""Check observer parsing without treating packets as authenticated application Data."""
import importlib.util
import csv
from pathlib import Path

from ndn.encoding import Name
import pytest


def analyzer():
    path = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di/analyze_transfer_packets.py'
    spec = importlib.util.spec_from_file_location('transfer_packets', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_data_and_lp_keep_typed_name_identity():
    module = analyzer()
    name = bytes(Name.to_bytes(Name.from_str('/producer/DATA/seg=0')))
    packet = bytes((6, len(name))) + name
    assert module.packet_name(packet) == (6, tuple(bytes(c) for c in Name.from_str('/producer/DATA/seg=0')))
    fragment = bytes((80, len(packet))) + packet
    assert module.packet_name(bytes((100, len(fragment))) + fragment) == module.packet_name(packet)
    assert module.packet_name(bytes((5, len(name))) + name)[0] == 5


@pytest.mark.parametrize('packet', [b'', b'\x06', b'\x06\x02\x07',
                                   b'\x64\x00', b'\x06\xfd\x00'])
def test_missing_or_truncated_data_does_not_become_a_timing_sample(packet):
    with pytest.raises((ValueError, IndexError)):
        analyzer().packet_name(packet)


def test_packet_join_uses_components_and_preserves_missing_observations(tmp_path):
    module = analyzer()
    objects = [dict(planned_name='/8=producer/8=object', producer_role='role',
                    producer_log='provider-0.log', output_ready_epoch_ms='1000000',
                    complete_epoch_ms='1000010', turn='1'),
               dict(planned_name='/producer/obj', producer_role='role',
                    producer_log='provider-0.log', output_ready_epoch_ms='1000000',
                    complete_epoch_ms='1000010', turn='1')]
    name = bytes(Name.to_bytes(Name.from_str('/producer/object/MANIFEST')))
    with (tmp_path / 'link-packets.tsv').open('w') as stream:
        writer = csv.DictWriter(stream, delimiter='\t', fieldnames=(
            'data.data', 'frame.time_epoch', 'ip.src', 'ip.dst'))
        writer.writeheader()
        for kind, stamp in ((5, '999.999'), (6, '1000.002')):
            writer.writerow({'data.data': (bytes((kind, len(name))) + name).hex(),
                             'frame.time_epoch': stamp, 'ip.src': 'a', 'ip.dst': 'b'})
    result = module.analyze(objects, tmp_path)
    first, missing = result['objects']
    assert first['ready_to_first_data_ms'] == 2
    assert first['last_data_to_admitted_ms'] == 8
    assert first['interest_seen_before_ready'] is True
    assert missing['first_data_epoch_ms'] is None
    assert result['packet_counts']['matched_packets'] == 2
