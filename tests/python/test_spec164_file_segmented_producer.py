import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "pythonWrapper"))

from ndnsf import (
    FileSegmentedObjectProducer,
    verify_detached_sha256_signature,
)


class FileSegmentedObjectProducerTests(unittest.TestCase):
    def test_file_geometry_is_bounded_and_public_key_is_exported(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = Path(temporary) / "payload.bin"
            payload.write_bytes(bytes(range(256)) * 33)
            old_home = os.environ.get("HOME")
            os.environ["HOME"] = temporary
            try:
                producer = FileSegmentedObjectProducer(
                    "/spec164/test/file",
                    str(payload),
                    signing_identity="/spec164/test/publisher",
                    max_segment_size=4096,
                    digest_signing=True,
                )
                self.assertEqual(producer.file_size, payload.stat().st_size)
                self.assertEqual(producer.segment_count, 3)
                self.assertGreater(len(producer.public_key_der), 100)
                self.assertEqual(producer.data_count, 0)
                self.assertEqual(producer.wire_bytes, 0)
            finally:
                if old_home is None:
                    os.environ.pop("HOME", None)
                else:
                    os.environ["HOME"] = old_home

    def test_detached_sha256_signature_is_verified_in_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private_key = root / "private.pem"
            public_key = root / "public.der"
            payload = root / "payload.bin"
            signature = root / "payload.sig"
            payload.write_bytes(b"spec164-detached-signature")
            subprocess.run([
                "openssl", "genpkey", "-algorithm", "RSA",
                "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private_key),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run([
                "openssl", "pkey", "-in", str(private_key), "-pubout",
                "-outform", "DER", "-out", str(public_key),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run([
                "openssl", "dgst", "-sha256", "-sign", str(private_key),
                "-out", str(signature), str(payload),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            self.assertTrue(verify_detached_sha256_signature(
                payload.read_bytes(),
                signature.read_bytes(),
                public_key.read_bytes(),
            ))
            self.assertFalse(verify_detached_sha256_signature(
                payload.read_bytes() + b"-corrupt",
                signature.read_bytes(),
                public_key.read_bytes(),
            ))


if __name__ == "__main__":
    unittest.main()
