#!/usr/bin/env python3
"""Python-ndn interoperability checks for the predictive Stream facade."""

from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest

from ndn.client_conf import default_keychain
from ndn.encoding import MetaInfo, Name, make_data, parse_data
from ndn.security import KeychainSqlite3
from ndn.security.tpm import TpmFile

from ndnsf import (
    ProviderSigningMetadata,
    SampleClassProfile,
    ServiceProvider,
    StreamConfig,
    make_signed_data,
    make_predictive_data_name,
)


class PythonNdnStreamPushTest(unittest.TestCase):
    def test_make_signed_data_remains_a_convenience_helper(self) -> None:
        """Keep the diagnostic helper exported without making it required."""

        with self.assertRaises(ValueError):
            make_signed_data(
                "/example/python-ndn/provider/diagnostic",
                b"payload",
                freshness_ms=-1,
            )

    def test_python_ndn_signs_canonical_predictive_wire(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ndnsf-python-ndn-") as root:
            root_path = Path(root)
            pib = root_path / "pib.db"
            tpm = root_path / "tpm"
            self.assertTrue(KeychainSqlite3.initialize(
                str(pib), "tpm-file", str(tpm)))
            keychain = KeychainSqlite3(str(pib), TpmFile(str(tpm)))
            identity = "/example/python-ndn/provider"
            keychain.touch_identity(identity)
            signer = keychain.get_signer({"identity": identity})

            name = make_predictive_data_name(
                "/example/python-ndn/provider/NDNSF/STREAM-MAP/camera",
                7,
                12,
            )
            wire = bytes(make_data(
                name,
                MetaInfo(freshness_period=300),
                b"jpeg-frame",
                signer=signer,
            ))

            parsed_name, meta, content, signature = parse_data(wire)
            self.assertEqual(Name.to_str(parsed_name), name)
            self.assertEqual(bytes(content), b"jpeg-frame")
            self.assertEqual(meta.freshness_period, 300)
            self.assertIsNotNone(signature.signature_info.key_locator)
            self.assertEqual(signature.signature_info.signature_type, 3)

    def test_signing_metadata_is_public_and_key_specific(self) -> None:
        provider = ServiceProvider.__new__(ServiceProvider)

        class FakeNative:
            provider_identity = "/example/provider"
            provider_signing_key_name = "/example/provider/KEY/ksk-ecdsa"
            provider_signing_certificate_name = (
                "/example/provider/KEY/ksk-ecdsa/self")
            provider_boot_epoch = "1"

        provider._native = FakeNative()
        metadata = provider.signing_metadata
        self.assertIsInstance(metadata, ProviderSigningMetadata)
        self.assertEqual(metadata.provider_identity, "/example/provider")
        self.assertIn("ksk-ecdsa", metadata.signing_key_name)
        self.assertEqual(
            provider.provider_signing_certificate_name,
            metadata.signing_certificate_name,
        )

    @unittest.skipUnless(
        os.environ.get("NDNSF_RUN_PYTHON_NDN_STREAM_INTEGRATION") == "1",
        "set NDNSF_RUN_PYTHON_NDN_STREAM_INTEGRATION=1 with a running MiniNDN controller",
    )
    def test_real_python_ndn_wire_is_accepted_by_stream_push(self) -> None:
        """Use the live C++ StreamPublisher, not a Python fake.

        The test is run by the MiniNDN integration launcher with the same
        PIB/TPM directory as the Provider. The Provider exposes only public
        signing names; python-ndn loads the matching private key locally.
        """

        provider = ServiceProvider(
            provider_id="",
            group="/example/python-ndn/group",
            controller="/example/python-ndn/controller",
            provider_prefix="/example/python-ndn/provider",
            trust_schema="examples/trust-schema.conf",
        )
        # The facade publisher shares the Provider Face event loop.  Keep one
        # ordinary service handler registered so the real NativeServiceProvider
        # can run that loop while StreamPublisher.push publishes the packet.
        provider.add_handler("/LiveStream/Dummy", lambda payload: payload)
        provider.start_background()
        stream = provider.create_stream(StreamConfig(
            stream_id="camera",
            data_prefix="/example/python-ndn/provider/camera",
            sample_period_ms=33.0,
            sample_classes=(SampleClassProfile("video", 1, 1),),
        ))
        try:
            descriptor = stream.start()
            metadata = provider.signing_metadata
            self.assertEqual(metadata.provider_identity,
                             descriptor.definition.provider)

            # ndn-cxx's PIB locator names its directory, while python-ndn's
            # SQLite backend receives the database file beneath that
            # directory.  The launcher supplies both views of the same store.
            config = default_keychain(
                os.environ.get("NDNSF_PYTHON_NDN_PIB",
                               os.environ["NDN_CLIENT_PIB"]),
                os.environ.get("NDNSF_PYTHON_NDN_TPM",
                               os.environ["NDN_CLIENT_TPM"]),
            )
            signer = config.get_signer({"key": metadata.signing_key_name})
            name = make_predictive_data_name(
                descriptor.definition.mapping_root,
                descriptor.definition.mapping_version,
                descriptor.checkpoint.next_expected_sample_id,
            )
            wire = bytes(make_data(
                name, MetaInfo(freshness_period=300), b"jpeg-frame",
                signer=signer,
            ))
            stream.push(wire)
            self.assertEqual(stream.status().frontiers.latest_produced, 0)
        finally:
            stream.stop()
            provider.stop()
            # Release pybind-held native objects before the interpreter starts
            # tearing down extension globals; this keeps the MiniNDN runner's
            # process exit deterministic after the real push assertion.
            del stream
            native = provider._native
            provider._native = None
            del native


if __name__ == "__main__":
    unittest.main()
