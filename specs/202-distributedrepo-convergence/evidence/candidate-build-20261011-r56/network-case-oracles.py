"""Independent case evidence checks supplementing the frozen runner."""
import json
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT / 'tests/minindn'))
import spec193_packet_oracle as packets
from ndn.encoding import Name, Component


def network_wires(path):
    # Same fail-closed IP/LP rules as the maintained metadata denial collector.
    output = subprocess.check_output([
        'tshark', '-2', '-o', 'ip.defragment:TRUE', '-r', str(path), '-T', 'fields',
        '-e', 'frame.number', '-e', 'data.data', '-e', 'ip.reassembled.data',
        '-e', 'ip.flags.mf', '-e', 'ip.frag_offset', '-e', 'ip.reassembled_in',
        '-e', 'tcp.len'], stderr=subprocess.DEVNULL).decode()
    for line in output.splitlines():
        fields = line.split('\t')
        if len(fields) != 7 or not fields[0].isdigit():
            raise RuntimeError('INCONCLUSIVE A05: malformed capture fields')
        frame, raw, reassembled, more, offset, completed_in, tcp_length = fields
        if tcp_length and int(tcp_length) > 0:
            raise RuntimeError('INCONCLUSIVE A05: unsupported TCP payload')
        if (more == '1' or (offset and int(offset) > 0)) and not reassembled:
            if completed_in:
                continue
            raise RuntimeError('INCONCLUSIVE A05: incomplete IP datagram')
        payloads = [value for value in raw.split(',') if value]
        if reassembled:
            datagram = bytes.fromhex(reassembled.replace(':', ''))
            if len(datagram) < 8 or struct.unpack('!H', datagram[4:6])[0] != len(datagram):
                raise RuntimeError('INCONCLUSIVE A05: invalid reassembled UDP')
            payloads = [datagram[8:].hex()]
        for payload in dict.fromkeys(payloads):
            try:
                wire = bytes.fromhex(payload.replace(':', ''))
                kind, body, _, end = packets._tlv(wire)
                if kind not in (5, 6, 100):
                    continue
                if end != len(wire):
                    raise ValueError('trailing network bytes')
                if kind == 100:
                    values = list(packets._fields(bytes(body)))
                    fragments = [value for kind, value, _ in values if kind == 80]
                    if not fragments:
                        continue
                    counts = [value for kind, value, _ in values if kind == 83]
                    indexes = [value for kind, value, _ in values if kind == 82]
                    if (len(fragments) != 1 or len(counts) > 1 or len(indexes) > 1 or
                        any(len(value) not in (1, 2, 4, 8) for value in counts + indexes) or
                        (counts and int.from_bytes(counts[0], 'big') != 1) or
                        (indexes and int.from_bytes(indexes[0], 'big') != 0)):
                        raise RuntimeError('INCONCLUSIVE A05: multi-fragment LP')
                    wire = fragments[0]
                    kind, _, _, end = packets._tlv(wire)
                    if end != len(wire) or kind not in (5, 6):
                        raise ValueError('invalid LP network payload')
                yield int(frame), kind, wire
            except (ValueError, IndexError, TypeError) as error:
                raise RuntimeError('INCONCLUSIVE A05: malformed NDN transport') from error


def reject_selection(name, caller):
    components = Name.from_str(name)
    prefix = Name.from_str(caller + '/NDNSF/SELECTION')
    if components[:len(prefix)] == prefix:
        raise RuntimeError('A05 caller published Selection: ' + name)


def principal(certificate_name):
    components = Name.from_str(certificate_name)
    positions = [i for i, component in enumerate(components)
                 if bytes(Component.get_value(component)) == b'KEY']
    assert len(positions) == 1
    return Name.to_str(components[:positions[0]])


def audit_a05(directory, manifest):
    scenario = manifest['scenario_result']
    assert scenario['case_id'] == 'A05' and scenario['status'] == 'PASS'
    expected = scenario['signed_packet_oracle']
    certificate, key, _, _ = packets._certificate(directory / 'identity/neu.cert')
    caller = principal(certificate)
    providers = {host: principal(packets._certificate(directory / ('identity/' + host + '.cert'))[0])
                 for host in expected['packets'] if host != 'neu'}
    request_name = expected['packets']['neu']['inner_name']
    components = Name.from_str(request_name)
    prefix = Name.from_str(caller + '/NDNSF/REQUEST')
    assert components[:len(prefix)] == prefix
    tail = components[len(prefix):]
    assert len(tail) >= 2 and bytes(Component.get_value(tail[-2])) == b'CAPABILITY'
    assert Name.to_str(tail[-1:]).lstrip('/') == scenario['request_id']
    service = Name.to_str(tail[:-2])
    actual = packets.verify_a05(directory, service, caller, providers,
                               scenario['capture_offsets'], scenario['request_id'], scenario['candidates'])
    assert actual == expected, 'signed Request/ACK evidence changed'
    root_name, root_key, _, _ = packets._certificate(directory / 'identity/root.cert')
    signers = []
    for host in manifest['hosts']:
        name, signing_key, signature, _ = packets._certificate(directory / ('identity/' + host + '.cert'))
        packets._verify_signature(signature, root_key, root_name, 'A05 group member certificate ' + host)
        signers.append((name, signing_key))
    publisher_prefixes = [caller + '/user'] + [provider + '/provider' for provider in providers.values()]
    captures = {}
    for capture in manifest['pcap_traces']:
        path = Path(capture['path'])
        assert packets._digest(path.read_bytes()) == capture['sha256']
        data_count = 0
        for packet in packets._denial_signed_packets(path, publisher_prefixes):
            reject_selection(packet['inner_name'], caller)
            data_count += 1
        publications = {}
        for frame, kind, wire in network_wires(path):
            if kind != 5:
                continue
            # Group members can relay caller mappings: verify every declared member.
            for signing_certificate, signing_key in signers:
                for identity, application, digest in packets._sync_interest_mappings(
                        wire, signing_certificate, signing_key, caller, '/example/repo/group'):
                    reject_selection(application, caller)
                    if identity in publications:
                        assert publications[identity]['name'] == application and publications[identity]['mapping_sha256'] == digest
                    else:
                        publications[identity] = {'name': application, 'mapping_sha256': digest, 'first_frame': frame}
        captures[capture['host']] = {'decoded_data_packets': data_count,
            'caller_signed_sync_publications': [dict(value, producer=identity[0], sequence=identity[1])
                                                for identity, value in publications.items()]}
    assert any(row['name'] == request_name for row in captures['neu']['caller_signed_sync_publications']), 'INCONCLUSIVE A05: caller Sync Request publication not observed'
    return {'status': 'A05_ORACLE_PASS', 'request_id': scenario['request_id'],
            'signed_request_ack': actual, 'selection_count': 0, 'captures': captures,
            'scope': 'No caller Selection Data or caller-signed Sync mapping in the full closed captures; positive Request/ACK signatures retained'}
