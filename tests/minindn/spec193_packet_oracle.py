"""Independent signed-packet oracle for Spec193 A05 MiniNDN discovery."""

from __future__ import annotations

import base64
import hashlib
import re
import struct
import subprocess
from pathlib import Path
from urllib.parse import quote

from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from ndn.encoding import Component, Name, parse_data, parse_tl_num


def _digest(wire: bytes) -> str:
    return hashlib.sha256(wire).hexdigest()


def _fields(wire: bytes):
    offset = 0
    while offset < len(wire):
        kind, value, encoded, offset = _tlv(wire, offset)
        yield kind, bytes(value), bytes(encoded)


def _mapping_entries(value: bytes, depth: int = 0):
    if depth > 64:
        raise RuntimeError("Sync mapping nesting exceeds its bound")
    fields = list(_fields(value))
    producers = [(v, w) for t, v, w in fields if t == 7]
    if len(producers) != 1:
        raise RuntimeError("Sync mapping must contain exactly one producer")
    producer = Name.to_str(Name.decode(producers[0][1])[0])
    for kind, entry, encoded in fields:
        if kind == 205:
            yield from _mapping_entries(entry, depth + 1)
        elif kind == 206:
            parts = list(_fields(entry))
            if [t for t, _, _ in parts] != [210, 7] or producer == "/":
                raise RuntimeError("Sync mapping entry has invalid sequence/name fields")
            sequence = list(_fields(parts[0][1]))
            if [t for t, _, _ in sequence] != [212, 214] or any(
                    len(v) not in (1, 2, 4, 8) for _, v, _ in sequence):
                raise RuntimeError("Sync mapping bootstrap/sequence is invalid")
            bootstrap, seq = (int.from_bytes(v, "big") for _, v, _ in sequence)
            application = Name.to_str(Name.decode(parts[1][2])[0])
            yield ((producer, bootstrap, seq), application,
                   _digest(producers[0][1] + encoded))
        elif kind != 7:
            raise RuntimeError("Sync mapping contains an unsupported field")


def _sync_interest_mappings(wire: bytes, certificate: str, key,
                            user: str, group: str):
    kind, value, _, end = _tlv(wire)
    if kind != 5 or end != len(wire):
        raise RuntimeError("Sync oracle requires one complete Interest")
    fields = list(_fields(value))
    names = [w for t, _, w in fields if t == 7]
    parameters = [(v, w) for t, v, w in fields if t == 36]
    if not parameters:
        return []
    if len(names) != 1 or len(parameters) != 1:
        raise RuntimeError("Sync Interest name/parameters are not unique")
    parameter_value, parameter_wire = parameters[0]
    if not parameter_value or _tlv(parameter_value)[0] != 6:
        return []
    name = Name.decode(names[0])[0]
    digests = [bytes(Component.get_value(c)) for c in name
               if Component.get_type(c) == 2]
    parameter_index = next(i for i, (t, _, _) in enumerate(fields) if t == 36)
    covered = b"".join(w for _, _, w in fields[parameter_index:])
    if len(digests) != 1 or digests[0] != hashlib.sha256(covered).digest():
        raise RuntimeError("Sync Interest ParametersSha256Digest mismatch")
    _, _, _, inner_end = _tlv(parameter_value)
    if inner_end != len(parameter_value):
        raise RuntimeError("Sync parameters contain trailing bytes")
    data_name, _, content, signature = parse_data(parameter_value)
    info = signature.signature_info
    if info is None or info.key_locator is None or info.key_locator.name is None:
        raise RuntimeError("Sync Data has no signing identity")
    if Name.to_str(info.key_locator.name) != certificate:
        return []  # Other certified group members may relay cached mappings.
    if Name.to_str(data_name) != group + "/v=3":
        raise RuntimeError("Caller Sync Data belongs to another group/version")
    _verify_signature(signature, key, certificate, "caller Sync Data")
    if content is None:
        raise RuntimeError("Sync Data contains no state/mapping content")
    result = []
    for kind, value, _ in _fields(bytes(content)):
        if kind == 205:
            for record in _mapping_entries(value):
                producer = record[0][0]
                producer_name = Name.from_str(producer)
                prefix = Name.from_str(user + "/user")
                if (len(producer_name) == len(prefix) + 1 and
                        producer_name[:len(prefix)] == prefix and
                        Component.get_type(producer_name[-1]) == Component.TYPE_GENERIC and
                        re.fullmatch(rb"[0-9]+", bytes(Component.get_value(producer_name[-1])))):
                    result.append(record)
    return result


def _sync_publications(path: Path, certificate: str, key, user: str, group: str):
    # Wireshark 3.2 has data.data and ip.reassembled.data, but no udp.payload field.
    output = subprocess.check_output(
        ["tshark", "-2", "-o", "ip.defragment:TRUE", "-r", str(path),
         "-T", "fields", "-e", "frame.number", "-e", "data.data",
         "-e", "ip.reassembled.data"], stderr=subprocess.DEVNULL).decode()
    publications = {}
    for line in output.splitlines():
        parts = line.split("\t")
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        payloads = [v for v in parts[1].split(",") if v]
        if parts[2]:
            reassembled = bytes.fromhex(parts[2].replace(":", ""))
            if len(reassembled) >= 8:
                udp_length = struct.unpack("!H", reassembled[4:6])[0]
                if udp_length == len(reassembled):
                    payloads.append(reassembled[8:].hex())
        seen_wire = set()
        for payload in payloads:
            wire = bytes.fromhex(payload.replace(":", ""))
            if wire in seen_wire:
                continue
            seen_wire.add(wire)
            try:
                kind, value, _, end = _tlv(wire)
                if kind == 100:
                    fragments = [v for t, v, _ in _fields(bytes(value)) if t == 80]
                    if len(fragments) != 1:
                        continue
                    wire = fragments[0]
                    kind, _, _, end = _tlv(wire)
                if kind != 5 or end != len(wire):
                    continue
            except (ValueError, IndexError):
                continue  # An incomplete NDN-LP fragment is not a signed publication.
            for identity, application, digest in _sync_interest_mappings(
                    wire, certificate, key, user, group):
                previous = publications.get(identity)
                if previous and (previous["name"], previous["mapping_sha256"]) != (application, digest):
                    raise RuntimeError("Conflicting signed Sync mapping publication")
                if previous is None:
                    publications[identity] = {"name": application, "mapping_sha256": digest,
                                              "first_frame": int(parts[0])}
    return publications


def verify_a21_sync_attempts(output: Path, service_prefix: str, user: str,
                             group: str, offline_offsets: dict,
                             native_attempts: list, expected_provider_order: list) -> dict:
    root_name, root_key, root_signature, root_digest = _certificate(output / "identity/root.cert")
    locator = Name.to_str(root_signature.signature_info.key_locator.name)
    if not root_name.startswith(locator + "/"):
        raise RuntimeError("A21 root certificate is not self-issued")
    _verify_signature(root_signature, root_key, locator, "A21 root certificate")
    certificate, key, signature, cert_digest = _certificate(output / "identity/arizona.cert")
    if not certificate.startswith(user + "/KEY/"):
        raise RuntimeError("A21 caller certificate identity mismatch")
    _verify_signature(signature, root_key, root_name, "A21 caller certificate")
    native = [line for line in native_attempts if line.startswith("SPEC193_A21_SECOND_ATTEMPT ")]
    providers = [line.split("provider=", 1)[1].split()[0] for line in native]
    if len(native) != 3 or len(set(providers)) != 3 or providers != expected_provider_order or any(
            "outcome=FAILED failureClass=timeout" not in line for line in native):
        raise RuntimeError("A21 native offline attempt ledger is not three distinct timeouts")
    prefix = user + "/NDNSF/REQUEST" + service_prefix + "/STATUS/"
    captures = {}
    for host in ("arizona", "memphis"):
        path = output / "pcap" / (host + ".pcap")
        threshold = _first_frame_at_or_after(path, offline_offsets[host])
        publications = _sync_publications(path, certificate, key, user, group)
        selected = [(identity, record) for identity, record in publications.items()
                    if record["name"].startswith(prefix) and record["first_frame"] >= threshold]
        selected.sort(key=lambda item: (item[0][1], item[0][2]))
        if len(selected) != 3 or len({identity[1] for identity, _ in selected}) != 1:
            raise RuntimeError("A21 capture lacks exactly three new offline STATUS publications: " + host)
        if len({identity[0] for identity, _ in selected}) != 1 or len(
                {record["name"] for _, record in selected}) != 3:
            raise RuntimeError("A21 offline publications repeat a Request or mix producer sessions")
        if [r["first_frame"] for _, r in selected] != sorted(r["first_frame"] for _, r in selected):
            raise RuntimeError("A21 capture publication order disagrees with sequence order")
        captures[host] = [{"producer": identity[0], "bootstrap": identity[1],
                           "sequence": identity[2], **record} for identity, record in selected]
    def comparable(records):
        return [{k: v for k, v in record.items() if k != "first_frame"} for record in records]
    if comparable(captures["arizona"]) != comparable(captures["memphis"]):
        raise RuntimeError("A21 caller/controller signed mappings differ")
    return {"status": "PASS", "root_certificate_sha256": root_digest,
            "caller_certificate_sha256": cert_digest, "captures": captures,
            "provider_attribution": "INFERRED_FROM_NATIVE_BLOCKING_ORDER",
            "native_provider_order": providers, "logical_offline_attempts": 3}


def _denial_signed_packets(path: Path, publisher_prefixes: list):
    """Conservative absence collector: unsupported transport is inconclusive."""
    output = subprocess.check_output(
        ["tshark", "-2", "-o", "ip.defragment:TRUE", "-r", str(path),
         "-T", "fields", "-e", "frame.number", "-e", "data.data",
         "-e", "ip.reassembled.data", "-e", "ip.flags.mf", "-e", "ip.frag_offset",
         "-e", "ip.reassembled_in", "-e", "tcp.len"], stderr=subprocess.DEVNULL).decode()
    producer_prefixes = [Name.from_str(prefix) for prefix in publisher_prefixes]
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) != 7 or not fields[0].isdigit():
            raise RuntimeError("INCONCLUSIVE metadata denial: malformed capture fields")
        _, raw, reassembled, more, offset, completed_in, tcp_length = fields
        if tcp_length and int(tcp_length) > 0:
            raise RuntimeError("INCONCLUSIVE metadata denial: TCP payload is unsupported")
        fragmented = more == "1" or (offset and int(offset) > 0)
        if fragmented and not reassembled:
            if completed_in:
                continue  # Two-pass IP dissection identifies the complete datagram's frame.
            raise RuntimeError("INCONCLUSIVE metadata denial: incomplete IP datagram")
        payloads = [item for item in raw.split(",") if item]
        if reassembled:
            datagram = bytes.fromhex(reassembled.replace(":", ""))
            if len(datagram) < 8 or struct.unpack("!H", datagram[4:6])[0] != len(datagram):
                raise RuntimeError("INCONCLUSIVE metadata denial: invalid reassembled UDP datagram")
            payloads = [datagram[8:].hex()]
        for payload in dict.fromkeys(payloads):
            try:
                wire = bytes.fromhex(payload.replace(":", ""))
                kind, body, _, end = _tlv(wire)
                if kind not in (5, 6, 100):
                    continue
                if end != len(wire):
                    raise ValueError("trailing network bytes")
                if kind == 100:
                    lp_fields = list(_fields(bytes(body)))
                    fragments = [value for kind, value, _ in lp_fields if kind == 80]
                    if not fragments:
                        continue  # LP control packet without network payload.
                    counts = [value for kind, value, _ in lp_fields if kind == 83]
                    indexes = [value for kind, value, _ in lp_fields if kind == 82]
                    if (len(fragments) != 1 or len(counts) > 1 or len(indexes) > 1 or
                            any(len(value) not in (1, 2, 4, 8) for value in counts + indexes) or
                            (counts and int.from_bytes(counts[0], "big") != 1) or
                            (indexes and int.from_bytes(indexes[0], "big") != 0)):
                        raise RuntimeError("INCONCLUSIVE metadata denial: multi-fragment LP packet")
                    wire = fragments[0]
                    kind, _, _, end = _tlv(wire)
                    if end != len(wire):
                        raise ValueError("trailing LP payload")
                if kind != 6:
                    continue
                outer_name, _, content, outer_signature = parse_data(wire)
                outer_uri = Name.to_str(outer_name)
                if "/NDNSF/" in outer_uri:
                    inner_wire, inner_name, inner_signature = wire, outer_name, outer_signature
                elif content and parse_tl_num(content)[0] == 6:
                    inner_wire = bytes(content)
                    inner_name, _, _, inner_signature = parse_data(inner_wire)
                else:
                    publisher_match = any(
                        len(outer_name) > len(prefix) and outer_name[:len(prefix)] == prefix
                        for prefix in producer_prefixes)
                    mapping_name = any(
                        Component.get_type(component) == Component.TYPE_GENERIC and
                        bytes(Component.get_value(component)) == b"MAPPING"
                        for component in outer_name)
                    if publisher_match and mapping_name:
                        try:
                            content_fields = list(_fields(bytes(content)))
                            mappings = [value for field_type, value, _ in content_fields
                                        if field_type == 205]
                            if (not mappings or any(field_type not in (201, 205)
                                                    for field_type, _, _ in content_fields)):
                                raise ValueError("unsupported mapping fields")
                            applications = []
                            for mapping in mappings:
                                applications.extend(record[1]
                                                    for record in _mapping_entries(mapping))
                            if not applications:
                                raise ValueError("empty mapping")
                        except (ValueError, IndexError, TypeError, RuntimeError) as error:
                            raise RuntimeError(
                                "INCONCLUSIVE metadata denial: malformed SVS mapping") from error
                        yield {"outer_wire": wire, "outer_name": outer_uri,
                               "outer_signature": outer_signature,
                               "svs_applications": applications}
                        continue
                    if publisher_match:
                        raise RuntimeError("INCONCLUSIVE metadata denial: unsupported SVS inner content")
                    continue
                yield {"outer_wire": wire, "outer_name": outer_uri,
                       "outer_signature": outer_signature, "inner_wire": inner_wire,
                       "inner_name": Name.to_str(inner_name), "inner_signature": inner_signature}
            except (ValueError, IndexError, TypeError) as error:
                raise RuntimeError("INCONCLUSIVE metadata denial: malformed NDN publication") from error


def verify_metadata_denial(output: Path, service_prefix: str, user: str,
                           data_name: str, providers: dict) -> dict:
    """Bind ungranted metadata probes to signed wire on caller and both servers."""
    root_name, root_key, root_signature, root_digest = _certificate(output / "identity/root.cert")
    root_locator = Name.to_str(root_signature.signature_info.key_locator.name)
    if not root_name.startswith(root_locator + "/"):
        raise RuntimeError("metadata denial root certificate is not self-issued")
    _verify_signature(root_signature, root_key, root_locator, "metadata denial root")
    certificate, key, signature, certificate_digest = _certificate(output / "identity/neu.cert")
    if not certificate.startswith(user + "/KEY/"):
        raise RuntimeError("metadata denial caller identity mismatch")
    _verify_signature(signature, root_key, root_name, "metadata denial caller")
    expected = {}
    forbidden = set()
    for operation in ("DATA_RECEIPT", "DATA_LOCATION"):
        request_id = "/spec193-server-auth-" + hashlib.sha256(
            (data_name + operation).encode()).hexdigest()[:32]
        service = service_prefix + "/" + operation
        expected[user + "/NDNSF/REQUEST" + service + request_id] = request_id
        for provider in providers.values():
            for message in ("ACK", "RESPONSE"):
                forbidden.add(provider + "/NDNSF/" + message + "/" +
                              quote(user, safe="") + service + request_id)
    captures = {}
    for host in ("neu", *providers):
        requests = {}
        for packet in _denial_signed_packets(
                output / "pcap" / (host + ".pcap"),
                [user + "/user", *[provider + "/provider" for provider in providers.values()]]):
            if "svs_applications" in packet:
                provider_publication = any(
                    packet["outer_name"].startswith(provider + "/provider/")
                    for provider in providers.values())
                if (provider_publication and
                        any(application in forbidden
                            for application in packet["svs_applications"])):
                    raise RuntimeError("ungranted metadata probe advertised ACK/Response: " + host)
                continue
            name = packet["inner_name"]
            if name in forbidden:
                raise RuntimeError("ungranted metadata probe received ACK/Response: " + host)
            if name not in expected:
                continue
            if not packet["outer_name"].startswith(user + "/"):
                raise RuntimeError("metadata denial publication namespace mismatch")
            _verify_signature(packet["outer_signature"], key, certificate, "metadata denial outer")
            _verify_signature(packet["inner_signature"], key, certificate, "metadata denial inner")
            digest = _digest(packet["inner_wire"])
            if name in requests and requests[name] != digest:
                raise RuntimeError("conflicting signed metadata denial request")
            requests[name] = digest
        if set(requests) != set(expected):
            raise RuntimeError("metadata denial capture lacks both signed requests: " + host)
        captures[host] = requests
    if any(requests != captures["neu"] for requests in captures.values()):
        raise RuntimeError("metadata denial requests differ across captures")
    return {"status": "PASS", "root_certificate_sha256": root_digest,
            "caller_certificate_sha256": certificate_digest,
            "signed_requests": captures, "ack_or_response_count": 0}


def _tlv(wire: bytes, offset: int = 0):
    field_type, size = parse_tl_num(wire, offset)
    value_start = offset + size
    value_length, size = parse_tl_num(wire, value_start)
    value_start += size
    end = value_start + value_length
    if end > len(wire):
        raise ValueError("truncated NDN TLV")
    return field_type, wire[value_start:end], wire[offset:end], end


def verify_original_data_route(output: Path, data_name: str,
                               wire_sha256: str, provider: str,
                               selected_host: str = "ucla",
                               failed_provider: str | None = None,
                               active_logs: dict[str, str] | None = None,
                               expect_single_serve: bool = False) -> dict:
    """Check original-name Interests and Data independently from client logs."""
    observed = {}
    for host in ("arizona", "ucla", "wustl"):
        interests = []
        data_digests = []
        capture = output / "pcap" / (host + ".pcap")
        dump = subprocess.check_output(
            ["tshark", "-r", str(capture), "-T", "fields", "-e", "data.data"],
            text=True, stderr=subprocess.DEVNULL)
        for line in dump.splitlines():
            if not line:
                continue
            try:
                packet = bytes.fromhex(line.replace(":", ""))
                packet_type, body, _, end = _tlv(packet)
                if end != len(packet):
                    continue
                if packet_type == 5:
                    fields = []
                    offset = 0
                    while offset < len(body):
                        field_type, value, encoded, offset = _tlv(body, offset)
                        fields.append((field_type, value, encoded))
                    if not fields or fields[0][0] != 7 or \
                            Name.to_str(Name.from_bytes(fields[0][2])) != data_name:
                        continue
                    hints = []
                    for field_type, value, _ in fields:
                        if field_type != 30:
                            continue
                        hint_offset = 0
                        while hint_offset < len(value):
                            hint_type, _, encoded, hint_offset = _tlv(value, hint_offset)
                            if hint_type == 7:
                                hints.append(Name.to_str(Name.from_bytes(encoded)))
                    interests.append(hints)
                elif packet_type == 6:
                    name, _, _, _ = parse_data(packet)
                    if Name.to_str(name) == data_name:
                        data_digests.append(_digest(packet))
            except (ValueError, TypeError, IndexError):
                continue
        observed[host] = {"hints": interests, "data_sha256": data_digests}
    expected_consumer_hints = ([[failed_provider], [provider]]
                               if failed_provider else [[provider]])
    if observed["arizona"]["hints"] != expected_consumer_hints or \
            observed["arizona"]["data_sha256"] != [wire_sha256]:
        raise RuntimeError("consumer capture lacks the hinted original Interest/Data")
    if selected_host not in ("ucla", "wustl"):
        raise RuntimeError("unknown selected Repo host")
    unselected_host = "wustl" if selected_host == "ucla" else "ucla"
    if observed[selected_host]["hints"] != [[provider]] or \
            observed[selected_host]["data_sha256"] != [wire_sha256]:
        raise RuntimeError("selected Repo capture lacks hinted original Interest/Data")
    if failed_provider:
        if observed[unselected_host]["hints"] != [[failed_provider]] or \
                observed[unselected_host]["data_sha256"]:
            raise RuntimeError("failed Repo traffic differs from one hinted Interest")
    elif observed[unselected_host]["hints"] or observed[unselected_host]["data_sha256"]:
        raise RuntimeError("unselected Repo received original Data traffic")
    served = ("repo-original-data-served name=" + data_name +
              " sha256=" + wire_sha256)
    selected_log = (Path(active_logs[selected_host]) if active_logs else
                    output / "logs" /
                    ("cpp-repo-" + selected_host + "-o17-original-seed.log"))
    unselected_log = (Path(active_logs[unselected_host]) if active_logs else
                      output / "logs" /
                      ("cpp-repo-" + unselected_host + "-o17-original-seed.log"))
    selected_serves = selected_log.read_text().count(served)
    if selected_serves == 0:
        raise RuntimeError("selected Repo handler did not attest serving the original Data")
    if expect_single_serve and selected_serves != 1:
        raise RuntimeError("cached read reached the selected Repo Data handler again")
    if (served in unselected_log.read_text()) != bool(failed_provider):
        raise RuntimeError("failed/unselected Repo handler evidence mismatches fault")
    return {"data_name": data_name, "provider_hint": provider,
            "wire_sha256": wire_sha256, "selected_host": selected_host,
            "observed": observed, "selected_repo_serves": selected_serves}


def verify_network_wrapper_calls(output: Path, reports: dict, service_prefix: str,
                                 user: str, provider_a: str, provider_b: str,
                                 caller_host: str = "neu", wrong_signer_request_id: str | None = None) -> dict:
    """Bind actual wrapper IDs to signed request/ACK/Selection/Response wire."""
    root_name, root_key, root_signature, _ = _certificate(output / "identity/root.cert")
    _verify_signature(root_signature, root_key,
                      Name.to_str(root_signature.signature_info.key_locator.name), "root certificate")
    certificates = {}
    for host, principal in ((caller_host, user), ("ucla", provider_a), ("wustl", provider_b)):
        name, key, signature, digest = _certificate(output / "identity" / (host + ".cert"))
        if not name.startswith(principal + "/KEY/"):
            raise RuntimeError("wrapper certificate principal mismatch: " + host)
        _verify_signature(signature, root_key, root_name, host + " certificate")
        certificates[principal] = (name, key, digest)
    prefixes = {
        "request": user + "/NDNSF/REQUEST" + service_prefix + "/",
        "ack": provider_a + "/NDNSF/ACK/" + quote(user, safe="") + service_prefix + "/",
        "selection": user + "/NDNSF/SELECTION/" + quote(provider_a, safe="") + service_prefix + "/",
        "response": provider_a + "/NDNSF/RESPONSE/" + quote(user, safe="") + service_prefix + "/",
    }
    forbidden = (user + "/NDNSF/SELECTION/" + quote(provider_b, safe="") + service_prefix + "/",
                 provider_b + "/NDNSF/RESPONSE/" + quote(user, safe="") + service_prefix + "/")
    broadcast_ack_prefix = provider_b + "/NDNSF/ACK/" + quote(user, safe="") + service_prefix + "/"
    broadcast_acks = {}
    captures = {}
    for host in (caller_host, "ucla", "wustl"):
        stages = {stage: {} for stage in prefixes}
        for packet in _signed_packets(output / "pcap" / (host + ".pcap"), 0):
            name = packet["inner_name"]
            if name.startswith(forbidden):
                raise RuntimeError("unselected Provider B received final Selection or returned Response")
            if name.startswith(broadcast_ack_prefix):
                cert_name, key, _ = certificates[provider_b]
                if not packet["outer_name"].startswith(provider_b + "/"):
                    raise RuntimeError("broadcast ACK namespace differs from signer")
                _verify_signature(packet["inner_signature"], key, cert_name, host + " inner broadcast ACK")
                _verify_signature(packet["outer_signature"], key, cert_name, host + " outer broadcast ACK")
                broadcast_acks[host + ":" + name] = _digest(packet["inner_wire"])
            for stage, prefix in prefixes.items():
                if not name.startswith(prefix):
                    continue
                is_fault = (stage == "response" and wrong_signer_request_id is not None and
                            name == prefixes["response"] + "STATUS" + wrong_signer_request_id)
                signer = provider_b if is_fault else (user if stage in ("request", "selection") else provider_a)
                cert_name, key, _ = certificates[signer]
                if not packet["outer_name"].startswith(signer + "/"):
                    raise RuntimeError("wrapper publication namespace differs from signer")
                _verify_signature(packet["inner_signature"], key, cert_name, host + " inner " + stage)
                _verify_signature(packet["outer_signature"], key, cert_name, host + " outer " + stage)
                stages[stage][name] = _digest(packet["inner_wire"])
        captures[host] = stages
    verified = []
    for phase in reports:
        for call in reports[phase]["calls"]:
            operation = {"FETCH_DATA": "FETCH", "FETCH_RANGE_DIRECT": "FETCH_RANGE",
                         "PUBLISH_INTENT": "STORE", "ARTIFACT_STORE": "STORE",
                         "ARTIFACT_STORE_RANGE": "STORE_RANGE", "ARTIFACT_COMMIT_RANGES": "COMMIT_RANGES",
                         "PUBLISH_STATUS": "STATUS", "LOCATION": "STATUS", "RECEIPT": "STATUS",
                         "AVAILABILITY": "STATUS", "ARTIFACT_FETCH": "FETCH_RANGE",
                         "RECEIPT_MALFORMED": "STATUS", "RECEIPT_WRONG_SIGNER": "STATUS"}.get(
                call["operation"], call["operation"])
            request_id = call["requestId"]
            if request_id in ("", "/"):
                candidates = [name for name in captures[caller_host]["request"]
                              if name.startswith(prefixes["request"] + operation + "/")]
                if len(candidates) != 1:
                    raise RuntimeError("catalog helper lacks one independently captured request: " + operation)
                request_id = "/" + candidates[0][len(prefixes["request"] + operation + "/"):]
            evidence = {}
            if call["operation"] == "RECEIPT_WRONG_SIGNER" and request_id != wrong_signer_request_id:
                raise RuntimeError("wrong-signer report ID differs from allowed injection")
            for stage in prefixes:
                expected = prefixes[stage] + operation + request_id
                peer = "wustl" if stage == "response" and call["operation"] == "RECEIPT_WRONG_SIGNER" else "ucla"
                for host in (caller_host, peer):
                    matched = {name: digest for name, digest in captures[host][stage].items()
                               if name == expected or name.startswith(expected + "/")}
                    if not matched:
                        raise RuntimeError("wrapper signed stage absent: " + host + " " + stage + " " + operation + request_id)
                    evidence[host + ":" + stage] = matched
                if evidence[caller_host + ":" + stage] != evidence[peer + ":" + stage]:
                    raise RuntimeError("wrapper signed wire differs across caller/Provider: " + stage + " " + operation)
            verified.append({"phase": phase, "method_label": call["operation"],
                             "operation": operation, "request_id": request_id, "wire": evidence})
    return {"status": "PASS", "signed_calls": verified,
            "unselected_provider": provider_b, "unselected_selections_or_responses": 0,
            "verified_broadcast_acks": broadcast_acks,
            "injected_wrong_signer_request_id": wrong_signer_request_id}


def verify_timeout_request(output: Path, report: dict, service_prefix: str,
                           user: str, providers: list[str], hosts: list[str]) -> dict:
    root_name, root_key, root_signature, _ = _certificate(output / "identity/root.cert")
    _verify_signature(root_signature, root_key,
                      Name.to_str(root_signature.signature_info.key_locator.name), "root certificate")
    certificate, key, signature, _ = _certificate(output / "identity/neu.cert")
    if not certificate.startswith(user + "/KEY/"):
        raise RuntimeError("timeout caller certificate principal mismatch")
    _verify_signature(signature, root_key, root_name, "timeout caller certificate")
    calls = report["calls"]
    if len(calls) != 1 or calls[0]["operation"] != "RECEIPT_TIMEOUT" or calls[0]["requestId"] in ("", "/"):
        raise RuntimeError("timeout report lacks one actual request ID")
    request_id = calls[0]["requestId"]
    request_name = user + "/NDNSF/REQUEST" + service_prefix + "/STATUS" + request_id
    forbidden = []
    for provider in providers:
        forbidden.extend([
            user + "/NDNSF/SELECTION/" + quote(provider, safe="") + service_prefix + "/STATUS" + request_id,
            provider + "/NDNSF/RESPONSE/" + quote(user, safe="") + service_prefix + "/STATUS" + request_id])
    forbidden.append(providers[0] + "/NDNSF/ACK/" + quote(user, safe="") + service_prefix + "/STATUS" + request_id)
    provider_b_certificate, provider_b_key, provider_b_signature, _ = _certificate(output / "identity/wustl.cert")
    if not provider_b_certificate.startswith(providers[1] + "/KEY/"):
        raise RuntimeError("timeout unselected Provider certificate principal mismatch")
    _verify_signature(provider_b_signature, root_key, root_name, "timeout unselected Provider certificate")
    broadcast_ack = providers[1] + "/NDNSF/ACK/" + quote(user, safe="") + service_prefix + "/STATUS" + request_id
    broadcast_acks = {}
    requests = {}
    for host in hosts:
        digests = set()
        for packet in _signed_packets(output / "pcap" / (host + ".pcap"), 0):
            name = packet["inner_name"]
            if any(name == value or name.startswith(value + "/") for value in forbidden):
                raise RuntimeError("timeout request has unexpected signed ACK/Selection/Response: " + host)
            if name == broadcast_ack:
                if not packet["outer_name"].startswith(providers[1] + "/"):
                    raise RuntimeError("timeout broadcast ACK publication namespace mismatch")
                _verify_signature(packet["inner_signature"], provider_b_key, provider_b_certificate, host + " timeout inner B ACK")
                _verify_signature(packet["outer_signature"], provider_b_key, provider_b_certificate, host + " timeout outer B ACK")
                broadcast_acks[host + ":" + name] = _digest(packet["inner_wire"])
            if name == request_name:
                if not packet["outer_name"].startswith(user + "/"):
                    raise RuntimeError("timeout Request publication namespace differs from caller")
                _verify_signature(packet["inner_signature"], key, certificate, host + " timeout inner Request")
                _verify_signature(packet["outer_signature"], key, certificate, host + " timeout outer Request")
                digests.add(_digest(packet["inner_wire"]))
        requests[host] = sorted(digests)
    if len(requests["neu"]) != 1:
        raise RuntimeError("timeout caller capture lacks one signed emitted Request")
    return {"status": "PASS", "request_id": request_id, "signed_requests": requests,
            "selected_provider_ack_count": 0, "signed_selection_response_count": 0,
            "verified_unselected_ack_observations": broadcast_acks}


def verify_cross_language_calls(output: Path, service_prefix: str,
                                callers: dict[str, str], providers: dict[str, str],
                                certificate_files: dict[str, str] | None = None,
                                required_operations: dict[str, set[str]] | None = None) -> dict:
    """Verify public native/binding chains independently from peer reports."""
    root_name, root_key, signature, _ = _certificate(output / "identity/root.cert")
    _verify_signature(signature, root_key,
                      Name.to_str(signature.signature_info.key_locator.name), "root certificate")
    certificates = {}
    for principal, host in {**callers, **providers}.items():
        certificate_path = Path(certificate_files[principal]) if certificate_files and principal in certificate_files else output / "identity" / (host + ".cert")
        name, key, signature, _ = _certificate(certificate_path)
        if not name.startswith(principal + "/KEY/"):
            raise RuntimeError("cross-language certificate principal mismatch")
        _verify_signature(signature, root_key, root_name, host + " certificate")
        certificates[principal] = (name, key)
    prefixes = []
    for caller in callers:
        prefixes.append((caller + "/NDNSF/REQUEST" + service_prefix + "/", "request", caller, None, caller))
        for provider in providers:
            prefixes.extend([
                (provider + "/NDNSF/ACK/" + quote(caller, safe="") + service_prefix + "/", "ack", caller, provider, provider),
                (caller + "/NDNSF/SELECTION/" + quote(provider, safe="") + service_prefix + "/", "selection", caller, provider, caller),
                (provider + "/NDNSF/RESPONSE/" + quote(caller, safe="") + service_prefix + "/", "response", caller, provider, provider)])
    captures = {}
    for host in set(callers.values()) | set(providers.values()):
        stages = {}
        for packet in _signed_packets(output / "pcap" / (host + ".pcap"), 0):
            for prefix, stage, caller, provider, signer in prefixes:
                if not packet["inner_name"].startswith(prefix):
                    continue
                tail = packet["inner_name"][len(prefix):]
                if not packet["outer_name"].startswith(signer + "/"):
                    raise RuntimeError("cross-language publication namespace differs from signer")
                cert_name, key = certificates[signer]
                _verify_signature(packet["inner_signature"], key, cert_name, host + " inner " + stage)
                _verify_signature(packet["outer_signature"], key, cert_name, host + " outer " + stage)
                binding = (stage, caller, provider, tail)
                stages.setdefault(binding, set()).add(_digest(packet["inner_wire"]))
        captures[host] = stages
    chains = []
    operations = {caller: set() for caller in callers}
    for caller, host in callers.items():
        for (stage, owner, provider, tail), response in captures[host].items():
            if stage != "response" or owner != caller:
                continue
            peer = providers[provider]
            evidence = {}
            for expected_stage in ("request", "ack", "selection", "response"):
                binding = (expected_stage, caller, None if expected_stage == "request" else provider, tail)
                local = captures[host].get(binding, set())
                remote = captures[peer].get(binding, set())
                if not local or local != remote or len(local) != 1:
                    raise RuntimeError("cross-language signed chain incomplete/different: " + str(binding))
                evidence[expected_stage] = sorted(local)
            operation = tail.split("/", 1)[0]
            operations[caller].add(operation)
            chains.append({"caller": caller, "provider": provider, "request": tail, "wire": evidence})
    for caller in callers:
        required = required_operations[caller] if required_operations else {"STORE", "FETCH_RANGE", "STATUS"}
        if not required.issubset(operations[caller]):
            raise RuntimeError("cross-language caller lacks publish/read/receipt network chains: " + caller)
    return {"status": "PASS", "signed_chains": chains,
            "operations": {caller: sorted(values) for caller, values in operations.items()}}


def verify_local_namespace_absent(output: Path, prefix: str, hosts: list[str]) -> dict:
    """Inspect packet names without using the local driver's PASS marker."""
    examined = {}
    for host in hosts:
        capture = output / "pcap" / (host + ".pcap")
        dump = subprocess.check_output(
            ["tshark", "-r", str(capture), "-T", "fields", "-e", "data.data"],
            text=True, stderr=subprocess.DEVNULL)
        count = 0
        for line in dump.splitlines():
            if not line:
                continue
            try:
                packet = bytes.fromhex(line.replace(":", ""))
                packet_type, body, _, _ = _tlv(packet)
                if packet_type == 100:
                    offset = 0
                    while offset < len(body):
                        field_type, value, _, offset = _tlv(body, offset)
                        if field_type == 80:
                            packet = value
                            break
                    packet_type, body, _, _ = _tlv(packet)
                if packet_type not in (5, 6):
                    continue
                name_type, _, name_wire, _ = _tlv(body)
                if name_type != 7:
                    continue
                name = Name.to_str(Name.from_bytes(name_wire))
                count += 1
                if prefix in name:
                    raise RuntimeError("local namespace appeared on network: " + host + " " + name)
                if packet_type == 6:
                    _, _, content, _ = parse_data(packet)
                    if content is not None and parse_tl_num(content)[0] == 6:
                        inner_name, _, _, _ = parse_data(bytes(content))
                        if prefix in Name.to_str(inner_name):
                            raise RuntimeError("local namespace appeared in signed publication: " + host)
            except (ValueError, TypeError, IndexError):
                continue
        if count == 0:
            raise RuntimeError("no decodable NDN packets in local-case capture: " + host)
        examined[host] = count
    return {"status": "PASS", "namespace": prefix, "ndn_packets_examined": examined,
            "matching_packets": 0}


def _certificate(path: Path):
    armored = "".join(line.strip() for line in path.read_text().splitlines()
                       if not line.startswith("-----"))
    wire = base64.b64decode(armored, validate=True)
    name, _, content, signature = parse_data(wire)
    if content is None:
        raise RuntimeError("certificate has no public key: " + str(path))
    key = serialization.load_der_public_key(bytes(content), default_backend())
    return Name.to_str(name), key, signature, _digest(wire)


def _verify_signature(signature, key, expected_locator: str, label: str) -> None:
    info = signature.signature_info
    if info is None or info.signature_type != 1 or info.key_locator is None or \
            info.key_locator.name is None or signature.signature_value_buf is None:
        raise RuntimeError(label + " lacks an RSA-SHA256 signature and KeyLocator")
    locator = Name.to_str(info.key_locator.name)
    if locator != expected_locator:
        raise RuntimeError(label + " KeyLocator differs from the pinned certificate")
    covered = b"".join(bytes(part) for part in signature.signature_covered_part)
    key.verify(bytes(signature.signature_value_buf), covered,
               padding.PKCS1v15(), hashes.SHA256())


def _first_frame_at_or_after(path: Path, minimum_offset: int) -> int:
    with path.open("rb") as stream:
        magic = stream.read(4)
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            byte_order = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            byte_order = ">"
        else:
            raise RuntimeError("A05 capture is not a classic pcap file: " + str(path))
        if len(stream.read(20)) != 20:
            raise RuntimeError("A05 capture has an incomplete pcap header")
        frame_number = 1
        while True:
            record_offset = stream.tell()
            header = stream.read(16)
            if not header:
                break
            if len(header) != 16:
                raise RuntimeError("A05 capture has an incomplete packet header")
            _, _, captured_length, _ = struct.unpack(byte_order + "IIII", header)
            if captured_length == 0 or captured_length > 262144:
                raise RuntimeError("A05 capture has an invalid packet length")
            if len(stream.read(captured_length)) != captured_length:
                raise RuntimeError("A05 capture has an incomplete packet body")
            if record_offset >= minimum_offset:
                return frame_number
            frame_number += 1
    raise RuntimeError("A05 capture has no complete frame after the case offset")


def _signed_packets(path: Path, minimum_offset: int):
    first_frame = _first_frame_at_or_after(path, minimum_offset)
    output = subprocess.check_output(
        ["tshark", "-r", str(path), "-T", "fields", "-e", "frame.number",
         "-e", "data.data"],
        text=True, stderr=subprocess.DEVNULL)
    for line in output.splitlines():
        fields = line.split("\t", 1)
        if len(fields) != 2 or not fields[0].isdigit() or \
                int(fields[0]) < first_frame:
            continue
        hex_wire = fields[1]
        if not hex_wire:
            continue
        try:
            outer_wire = bytes.fromhex(hex_wire.replace(":", ""))
            if parse_tl_num(outer_wire)[0] != 6:
                continue
            outer_name, _, content, outer_signature = parse_data(outer_wire)
            if content is None or parse_tl_num(content)[0] != 6:
                continue
            inner_wire = bytes(content)
            inner_name, _, _, inner_signature = parse_data(inner_wire)
        except (ValueError, IndexError, TypeError):
            continue
        yield {"outer_wire": outer_wire, "outer_name": Name.to_str(outer_name),
               "outer_signature": outer_signature, "inner_wire": inner_wire,
               "inner_name": Name.to_str(inner_name),
               "inner_signature": inner_signature}


def verify_a05(output: Path, service_prefix: str, user: str,
               providers: dict, capture_offsets: dict, request_id: str,
               candidates: list) -> dict:
    """Verify one request and its two ACKs from packet wire and pinned certs."""
    identity = output / "identity"
    root_name, root_key, root_sig, root_digest = _certificate(identity / "root.cert")
    root_locator = Name.to_str(root_sig.signature_info.key_locator.name)
    if not root_name.startswith(root_locator + "/"):
        raise RuntimeError("root certificate is not self-issued")
    _verify_signature(root_sig, root_key, root_locator, "root certificate")

    certificates = {}
    for host, principal in {"neu": user, **providers}.items():
        name, key, sig, cert_digest = _certificate(identity / (host + ".cert"))
        if not name.startswith(principal + "/KEY/"):
            raise RuntimeError("certificate identity differs from expected host " + host)
        _verify_signature(sig, root_key, root_name, host + " certificate")
        certificates[host] = (name, key, cert_digest)

    request_prefix = user + "/NDNSF/REQUEST" + service_prefix + "/CAPABILITY/"
    escaped_user = quote(user, safe="")
    ack_prefixes = {
        host: principal + "/NDNSF/ACK/" + escaped_user +
        service_prefix + "/CAPABILITY/"
        for host, principal in providers.items()
    }
    expected = {"neu": request_prefix, **ack_prefixes}
    packet_sets = {}
    for capture_host in ("neu", *providers):
        relevant = {host: {} for host in expected}
        for packet in _signed_packets(output / "pcap" / (capture_host + ".pcap"),
                                      capture_offsets[capture_host]):
            for owner, prefix in expected.items():
                if not packet["inner_name"].startswith(prefix):
                    continue
                packet_request_id = packet["inner_name"][len(prefix):]
                if not packet_request_id or "/" in packet_request_id:
                    raise RuntimeError("malformed A05 request ID in signed packet")
                cert_name, key, _ = certificates[owner]
                if not packet["outer_name"].startswith(
                        (user if owner == "neu" else providers[owner]) + "/"):
                    raise RuntimeError("SVS publication namespace differs from signer")
                _verify_signature(packet["outer_signature"], key, cert_name,
                                  capture_host + " outer " + owner)
                _verify_signature(packet["inner_signature"], key, cert_name,
                                  capture_host + " inner " + owner)
                inner_digest = _digest(packet["inner_wire"])
                relevant[owner][inner_digest] = {
                    "request_id": packet_request_id,
                    "inner_name": packet["inner_name"],
                    "outer_name": packet["outer_name"],
                    "inner_sha256": inner_digest,
                    "outer_sha256": _digest(packet["outer_wire"]),
                    "certificate_sha256": certificates[owner][2],
                }
        if any(len(packets) != 1 for packets in relevant.values()):
            raise RuntimeError("A05 capture lacks exactly one signed request and ACK per principal on " +
                               capture_host)
        packet_sets[capture_host] = {owner: next(iter(packets.values()))
                                     for owner, packets in relevant.items()}

    canonical = packet_sets["neu"]
    request_ids = {item["request_id"] for item in canonical.values()}
    if len(request_ids) != 1:
        raise RuntimeError("A05 signed request and ACK IDs do not match")
    if request_ids != {request_id}:
        raise RuntimeError("A05 signed request ID differs from API invocation")
    expected_providers = set(providers.values())
    if {candidate["provider"] for candidate in candidates} != expected_providers:
        raise RuntimeError("A05 API returned a different provider set")
    for host, provider in providers.items():
        candidate = next(item for item in candidates if item["provider"] == provider)
        if candidate["ack_wire_digest"] != "sha256:" + canonical[host]["inner_sha256"]:
            raise RuntimeError("A05 accepted ACK digest differs from signed packet: " + host)
        if candidate["signer_key_locator"] != certificates[host][0]:
            raise RuntimeError("A05 accepted ACK locator differs from signer certificate: " + host)
    for capture_host, packets in packet_sets.items():
        if {owner: item["inner_sha256"] for owner, item in packets.items()} != \
                {owner: item["inner_sha256"] for owner, item in canonical.items()}:
            raise RuntimeError("A05 signed packet differs across host captures: " + capture_host)
    return {"request_id": request_ids.pop(), "root_certificate_sha256": root_digest,
            "packets": canonical,
            "observed_on_hosts": list(packet_sets)}


def verify_object_capability(output: Path, service_prefix: str, user: str,
                             provider: str, other_provider: str,
                             capture_offsets: dict, request_id: str,
                             accepted_ack_digest: str,
                             accepted_locator: str) -> dict:
    """Bind the decrypted object capability to one independently signed ACK."""
    identity = output / "identity"
    root_name, root_key, root_sig, root_digest = _certificate(identity / "root.cert")
    root_locator = Name.to_str(root_sig.signature_info.key_locator.name)
    if not root_name.startswith(root_locator + "/"):
        raise RuntimeError("object capability root certificate is not self-issued")
    _verify_signature(root_sig, root_key, root_locator, "root certificate")
    certificates = {}
    for host, principal in {"neu": user, "ucla": provider,
                            "wustl": other_provider}.items():
        name, key, sig, cert_digest = _certificate(identity / (host + ".cert"))
        if not name.startswith(principal + "/KEY/"):
            raise RuntimeError("object capability certificate identity mismatch: " + host)
        _verify_signature(sig, root_key, root_name, host + " certificate")
        certificates[host] = (name, key, cert_digest)

    request_id = request_id.lstrip("/")
    if not request_id or "/" in request_id:
        raise RuntimeError("object capability API request ID is malformed")
    request_prefix = user + "/NDNSF/REQUEST" + service_prefix + "/CAPABILITY/"
    escaped_user = quote(user, safe="")
    ack_prefix = provider + "/NDNSF/ACK/" + escaped_user + \
        service_prefix + "/CAPABILITY/"
    other_ack_prefix = other_provider + "/NDNSF/ACK/" + escaped_user + \
        service_prefix + "/CAPABILITY/" + request_id
    packets_by_host = {}
    other_ack_digests = {}
    for capture_host in ("neu", "ucla", "wustl"):
        relevant = {"neu": {}, "ucla": {}}
        for packet in _signed_packets(output / "pcap" / (capture_host + ".pcap"),
                                      capture_offsets[capture_host]):
            if packet["inner_name"] == other_ack_prefix:
                cert_name, key, _ = certificates["wustl"]
                if not packet["outer_name"].startswith(other_provider + "/"):
                    raise RuntimeError("other object Repo ACK namespace/signer mismatch")
                _verify_signature(packet["outer_signature"], key, cert_name,
                                  capture_host + " outer other Repo")
                _verify_signature(packet["inner_signature"], key, cert_name,
                                  capture_host + " inner other Repo")
                other_ack_digests.setdefault(capture_host, set()).add(
                    _digest(packet["inner_wire"]))
            for owner, prefix in (("neu", request_prefix), ("ucla", ack_prefix)):
                if packet["inner_name"] != prefix + request_id:
                    continue
                cert_name, key, _ = certificates[owner]
                expected_outer = user if owner == "neu" else provider
                if not packet["outer_name"].startswith(expected_outer + "/"):
                    raise RuntimeError("object capability SVS namespace/signer mismatch")
                _verify_signature(packet["outer_signature"], key, cert_name,
                                  capture_host + " outer " + owner)
                _verify_signature(packet["inner_signature"], key, cert_name,
                                  capture_host + " inner " + owner)
                digest = _digest(packet["inner_wire"])
                relevant[owner][digest] = {
                    "inner_sha256": digest,
                    "inner_name": packet["inner_name"],
                    "outer_sha256": _digest(packet["outer_wire"]),
                    "certificate_sha256": certificates[owner][2],
                }
        if capture_host in ("neu", "ucla") and \
                any(len(entries) != 1 for entries in relevant.values()):
            raise RuntimeError("object capability capture lacks signed request/ACK on " +
                               capture_host)
        packets_by_host[capture_host] = {
            owner: next(iter(entries.values())) for owner, entries in relevant.items()
            if entries}
    canonical = packets_by_host["neu"]
    if {owner: packet["inner_sha256"] for owner, packet in
            packets_by_host["ucla"].items()} != \
            {owner: packet["inner_sha256"] for owner, packet in canonical.items()}:
        raise RuntimeError("object capability signed packets differ across hosts")
    if accepted_ack_digest != "sha256:" + canonical["ucla"]["inner_sha256"]:
        raise RuntimeError("object capability accepted ACK digest differs from packet")
    if accepted_locator != certificates["ucla"][0]:
        raise RuntimeError("object capability accepted ACK locator differs from certificate")
    all_other_digests = set().union(*other_ack_digests.values()) if \
        other_ack_digests else set()
    if len(all_other_digests) > 1:
        raise RuntimeError("other object Repo ACK differs across host captures")
    return {"request_id": request_id, "root_certificate_sha256": root_digest,
            "packets": canonical, "observed_on_hosts": list(packets_by_host),
            "other_repo_ack_sha256": next(iter(all_other_digests), None),
            "accepted_provider": provider}
