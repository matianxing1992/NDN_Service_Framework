"""Offline signature and mapping negatives; no MiniNDN or production behavior simulation."""

import struct
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from ndn.encoding import InterestParam, MetaInfo, Name, make_data, make_interest
from ndn.security.signer import Sha256WithRsaSigner

from spec193_packet_oracle import (_mapping_entries, _sync_interest_mappings, _sync_publications,
                                  _tlv, verify_a21_sync_attempts, verify_metadata_denial)


def tlv(kind, value):
    length = bytes([len(value)]) if len(value) < 253 else b"\xfd" + struct.pack("!H", len(value))
    return bytes([kind]) + length + value


class SyncMappingOracleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.user = "/test/user"
        cls.producer = cls.user + "/user/508"
        cls.group = "/test/group"
        cls.certificate = cls.user + "/KEY/test/issuer/v=1"
        private = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                          backend=default_backend())
        cls.key = private.public_key()
        cls.signer = Sha256WithRsaSigner(cls.certificate, private.private_bytes(
            serialization.Encoding.DER, serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))

    def mapping(self, seq=6, application=None, producer=None):
        if application is None:
            application = self.user + "/NDNSF/REQUEST/Repo/STATUS/request-" + str(seq)
        sequence = tlv(210, tlv(212, struct.pack("!I", 1234)) + tlv(214, bytes([seq])))
        entry = tlv(206, sequence + Name.to_bytes(Name.from_str(application)))
        return tlv(205, Name.to_bytes(Name.from_str(producer or self.producer)) + entry)

    def interest(self, content=None, signer=None):
        if content is None:
            content = tlv(201, b"") + self.mapping()
        data = bytes(make_data(self.group + "/v=3", MetaInfo(), content,
                               signer=signer or self.signer))
        return bytes(make_interest(self.group + "/sync", InterestParam(), app_param=data))

    def decode(self, wire):
        return _sync_interest_mappings(wire, self.certificate, self.key, self.user, self.group)

    def test_actual_rsa_signed_interest_and_mapping(self):
        records = self.decode(self.interest())
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0][0], (self.producer, 1234, 6))
        self.assertTrue(records[0][1].endswith("/request-6"))

    def test_parameters_digest_rejects_corrupt_signature_bytes(self):
        wire = bytearray(self.interest())
        wire[-1] ^= 1
        with self.assertRaisesRegex(RuntimeError, "ParametersSha256Digest"):
            self.decode(bytes(wire))

    def test_rsa_signature_rejects_tampering_even_with_recomputed_parameter_digest(self):
        wire = self.interest()
        _, value, _, _ = _tlv(wire)
        position = 0
        parameters = None
        while position < len(value):
            kind, field, _, position = _tlv(value, position)
            if kind == 36:
                parameters = bytearray(field)
        self.assertIsNotNone(parameters)
        parameters[-1] ^= 1
        tampered = bytes(make_interest(self.group + "/sync", InterestParam(),
                                       app_param=bytes(parameters)))
        with self.assertRaises(InvalidSignature):
            self.decode(tampered)

    def test_unpinned_keylocator_is_not_caller_evidence(self):
        other = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                        backend=default_backend())
        signer = Sha256WithRsaSigner("/test/other/KEY/test/issuer/v=1", other.private_bytes(
            serialization.Encoding.DER, serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))
        self.assertEqual(self.decode(self.interest(signer=signer)), [])

    def test_group_mismatch_rejected(self):
        data = bytes(make_data("/other/group/v=3", MetaInfo(), self.mapping(), signer=self.signer))
        with self.assertRaisesRegex(RuntimeError, "another group"):
            self.decode(bytes(make_interest(self.group + "/sync", InterestParam(), app_param=data)))

    def test_bootstrap_and_sequence_shape_and_nesting_bound(self):
        producer = Name.to_bytes(Name.from_str(self.user))
        malformed = producer + tlv(206, tlv(210, tlv(214, b"\x06")) + producer)
        with self.assertRaisesRegex(RuntimeError, "bootstrap/sequence"):
            list(_mapping_entries(malformed))
        nested = self.mapping()
        for _ in range(66):
            nested = tlv(205, Name.to_bytes(Name.from_str("/")) + nested)
        with self.assertRaisesRegex(RuntimeError, "nesting"):
            list(_mapping_entries(_tlv(nested)[1]))

    def test_truncated_interest_rejected(self):
        with self.assertRaises(ValueError):
            self.decode(self.interest()[:-1])

    def test_producer_must_use_component_validated_native_session_shape(self):
        for producer in (self.user, self.user + "/user/x", self.producer + "/extra", "/other/user/508"):
            self.assertEqual(self.decode(self.interest(content=self.mapping(producer=producer))), [])

    def capture_output(self, packets):
        return "".join(str(frame) + "\t" + wire.hex() + "\t\n"
                       for frame, wire in packets).encode()

    def test_retransmission_dedup_and_conflicting_signed_mapping(self):
        wire = self.interest()
        with patch("spec193_packet_oracle.subprocess.check_output",
                   return_value=self.capture_output([(1, wire), (11, wire)])):
            result = _sync_publications(Path("unused"), self.certificate, self.key, self.user, self.group)
            self.assertEqual(len(result), 1)
            self.assertEqual(next(iter(result.values()))["first_frame"], 1)
        conflict = self.interest(content=self.mapping(application="/different/request"))
        with patch("spec193_packet_oracle.subprocess.check_output",
                   return_value=self.capture_output([(1, wire), (2, conflict)])):
            with self.assertRaisesRegex(RuntimeError, "Conflicting"):
                _sync_publications(Path("unused"), self.certificate, self.key, self.user, self.group)

    def test_incomplete_lp_fragment_is_not_publication_evidence(self):
        incomplete = tlv(100, tlv(80, self.interest()[:30]))
        with patch("spec193_packet_oracle.subprocess.check_output",
                   return_value=self.capture_output([(1, incomplete)])):
            self.assertEqual(_sync_publications(Path("unused"), self.certificate,
                                               self.key, self.user, self.group), {})

    def public_certificates(self, directory):
        import base64
        root = rsa.generate_private_key(public_exponent=65537, key_size=2048,
                                       backend=default_backend())
        der = root.private_bytes(serialization.Encoding.DER,
                                 serialization.PrivateFormat.TraditionalOpenSSL,
                                 serialization.NoEncryption())
        root_name = "/test/root/KEY/root/self/v=1"
        root_signer = Sha256WithRsaSigner("/test/root/KEY/root", der)
        root_wire = bytes(make_data(root_name, MetaInfo(), root.public_key().public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo),
            signer=root_signer))
        issuer = Sha256WithRsaSigner(root_name, der)
        caller = bytes(make_data(self.certificate, MetaInfo(), self.key.public_bytes(
            serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo), signer=issuer))
        identity = directory / "identity"
        identity.mkdir()
        (identity / "root.cert").write_text(base64.b64encode(root_wire).decode())
        (identity / "arizona.cert").write_text(base64.b64encode(caller).decode())

    def verify(self, directory, packets, providers=None, controller_packets=None):
        native = ["SPEC193_A21_SECOND_ATTEMPT provider=" + p +
                  " outcome=FAILED failureClass=timeout" for p in (providers or ["C", "B", "A"])]
        with patch("spec193_packet_oracle.subprocess.check_output",
                   side_effect=[self.capture_output(packets), self.capture_output(
                       packets if controller_packets is None else controller_packets)]), \
                patch("spec193_packet_oracle._first_frame_at_or_after", return_value=10):
            return verify_a21_sync_attempts(directory, "/Repo", self.user, self.group,
                                           {"arizona": 0, "memphis": 0}, native, ["C", "B", "A"])

    def test_whole_capture_first_seen_excludes_piggyback_and_enforces_distinct_requests(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.public_certificates(directory)
            old = self.interest(content=self.mapping(seq=1))
            packets = [(1, old), (10, old)] + [(seq + 10, self.interest(content=self.mapping(seq=seq)))
                                             for seq in (6, 7, 8)]
            self.assertEqual(self.verify(directory, packets)["logical_offline_attempts"], 3)
            duplicate = [(seq + 10, self.interest(content=self.mapping(
                seq=seq, application=self.user + "/NDNSF/REQUEST/Repo/STATUS/same"))) for seq in (6, 7, 8)]
            with self.assertRaisesRegex(RuntimeError, "repeat a Request"):
                self.verify(directory, duplicate)
            with self.assertRaisesRegex(RuntimeError, "native offline"):
                self.verify(directory, packets, ["X", "B", "A"])
            with self.assertRaisesRegex(RuntimeError, "native offline"):
                self.verify(directory, packets, ["C", "A", "B"])
            (directory / "identity/root.cert").unlink()
            with self.assertRaises(FileNotFoundError):
                self.verify(directory, packets)

    def test_metadata_denial_signed_requests_and_fail_closed_negatives(self):
        from urllib.parse import quote
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.public_certificates(directory)
            (directory / "identity/arizona.cert").rename(directory / "identity/neu.cert")
            data_name = "/test/original"
            providers = {"ucla": "/test/A", "wustl": "/test/B"}
            ids = ["/spec193-server-auth-" + hashlib.sha256(
                (data_name + operation).encode()).hexdigest()[:32]
                   for operation in ("DATA_RECEIPT", "DATA_LOCATION")]
            def packet(name, seq, corrupt_inner=False):
                inner = bytearray(make_data(name, MetaInfo(), b"encrypted-probe", signer=self.signer))
                if corrupt_inner:
                    inner[-1] ^= 1
                return bytes(make_data(self.producer + "/seq=" + str(seq), MetaInfo(),
                                       bytes(inner), signer=self.signer))
            packets = [packet(self.user + "/NDNSF/REQUEST/Repo/" + operation + request_id, seq)
                       for seq, (operation, request_id) in enumerate(
                           zip(("DATA_RECEIPT", "DATA_LOCATION"), ids), 1)]
            def dump(values):
                return "".join(str(i) + "\t" + wire.hex() + "\t\t0\t0\t\t\n"
                               for i, wire in enumerate(values, 1)).encode()
            def verify(captures, transport_outputs=None):
                with patch("spec193_packet_oracle._first_frame_at_or_after", return_value=1), \
                        patch("spec193_packet_oracle.subprocess.check_output",
                              side_effect=transport_outputs or [dump(values) for values in captures]):
                    return verify_metadata_denial(directory, "/Repo", self.user, data_name, providers)
            self.assertEqual(verify([packets] * 3)["ack_or_response_count"], 0)

            def mapping_publication(application):
                producer = "/test/A/provider/700"
                mapping = self.mapping(seq=4, application=application, producer=producer)
                return bytes(make_data(
                    producer + "/test/group/t=1234/MAPPING/seq=1/seq=4",
                    MetaInfo(), mapping, signer=self.signer))

            unrelated_mapping = mapping_publication(
                "/test/A/NDNSF/ACK/" + quote(self.user, safe="") +
                "/Repo/DATA_RECEIPT/previous-request")
            self.assertEqual(
                verify([packets, packets + [unrelated_mapping], packets])[
                    "ack_or_response_count"], 0)
            advertised_ack = mapping_publication(
                "/test/A/NDNSF/ACK/" + quote(self.user, safe="") +
                "/Repo/DATA_RECEIPT" + ids[0])
            with self.assertRaisesRegex(RuntimeError, "ACK/Response"):
                verify([packets, packets + [advertised_ack], packets])

            with self.assertRaisesRegex(RuntimeError, "lacks both"):
                verify([packets, packets, packets[:1]])
            ack = packet("/test/A/NDNSF/ACK/" + quote(self.user, safe="") +
                         "/Repo/DATA_RECEIPT" + ids[0], 3)
            with self.assertRaisesRegex(RuntimeError, "ACK/Response"):
                verify([packets, packets + [ack], packets])
            wrapped_ack = tlv(100, tlv(80, ack))
            with self.assertRaisesRegex(RuntimeError, "ACK/Response"):
                verify([packets, packets + [wrapped_ack], packets])
            fragmented_ack = tlv(100, tlv(81, b"\x01") + tlv(82, b"\x00") +
                                 tlv(83, b"\x02") + tlv(80, ack[:len(ack) // 2]))
            with self.assertRaisesRegex(RuntimeError, "INCONCLUSIVE.*multi-fragment"):
                verify([packets, packets + [fragmented_ack], packets])
            with self.assertRaisesRegex(RuntimeError, "INCONCLUSIVE.*malformed"):
                verify([packets, packets + [ack[:-1]], packets])
            malformed_inner = bytearray(make_data(
                "/test/A/NDNSF/ACK/" + quote(self.user, safe="") +
                "/Repo/DATA_RECEIPT" + ids[0], MetaInfo(), b"reply", signer=self.signer))
            malformed_inner[0] = 7
            malformed_publication = bytes(make_data(
                "/test/A/provider/700/seq=2", MetaInfo(), bytes(malformed_inner), signer=self.signer))
            with self.assertRaisesRegex(RuntimeError, "INCONCLUSIVE.*SVS inner"):
                verify([packets, packets + [malformed_publication], packets])
            udp = struct.pack("!HHHH", 6363, 6363, len(ack) + 8, 0) + ack
            reassembled = dump(packets) + ("3\t\t" + udp.hex() + "\t0\t1\t\t\n").encode()
            with self.assertRaisesRegex(RuntimeError, "ACK/Response"):
                verify([], [dump(packets), reassembled, dump(packets)])
            incomplete_ip = dump(packets) + (
                "3\t" + ack[:20].hex() + "\t\t1\t0\t\t\n").encode()
            with self.assertRaisesRegex(RuntimeError, "INCONCLUSIVE.*incomplete IP"):
                verify([], [dump(packets), incomplete_ip, dump(packets)])
            tcp = dump(packets) + ("3\t" + ack.hex() + "\t\t0\t0\t\t10\n").encode()
            with self.assertRaisesRegex(RuntimeError, "INCONCLUSIVE.*TCP"):
                verify([], [dump(packets), tcp, dump(packets)])
            corrupt = packet(self.user + "/NDNSF/REQUEST/Repo/DATA_RECEIPT" + ids[0],
                             1, corrupt_inner=True)
            with self.assertRaises(InvalidSignature):
                verify([[corrupt, packets[1]], packets, packets])

    def test_missing_caller_corrupt_chain_and_cross_capture_disagreement(self):
        import base64
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.public_certificates(directory)
            packets = [(seq + 10, self.interest(content=self.mapping(seq=seq)))
                       for seq in (6, 7, 8)]
            differing = [(seq + 10, self.interest(content=self.mapping(
                seq=seq, application=self.user + "/NDNSF/REQUEST/Repo/STATUS/other-" + str(seq))))
                         for seq in (6, 7, 8)]
            with self.assertRaisesRegex(RuntimeError, "caller/controller"):
                self.verify(directory, packets, controller_packets=differing)
            with self.assertRaisesRegex(RuntimeError, "exactly three"):
                self.verify(directory, packets, controller_packets=packets[:2])
            caller_path = directory / "identity/arizona.cert"
            original = caller_path.read_text()
            wire = bytearray(base64.b64decode(original))
            wire[-1] ^= 1
            caller_path.write_text(base64.b64encode(wire).decode())
            with self.assertRaises(InvalidSignature):
                self.verify(directory, packets)
            caller_path.unlink()
            with self.assertRaises(FileNotFoundError):
                self.verify(directory, packets)


if __name__ == "__main__":
    unittest.main()
