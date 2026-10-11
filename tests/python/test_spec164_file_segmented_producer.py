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
            isolated_keys = ("NDNSF_FILE_PUBLICATION_CACHE",)
            previous = {key: os.environ.get(key) for key in isolated_keys}
            os.environ["HOME"] = temporary
            os.environ["NDNSF_FILE_PUBLICATION_CACHE"] = temporary + "/cache"
            try:
                command_env = dict(os.environ)
                command_env.pop("LD_PRELOAD", None)
                subprocess.run(["ndnsec", "key-gen", "-t", "r", "/spec164/test/publisher"],
                               env=command_env, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=15)
                producer = FileSegmentedObjectProducer(
                    "/spec164/test/publisher/NDNSF-DI/blob/8=name%3Dfile",
                    str(payload),
                    signing_identity="/spec164/test/publisher",
                    max_segment_size=4096,
                    digest_signing=False,
                )
                self.assertEqual(producer.file_size, payload.stat().st_size)
                self.assertEqual(producer.segment_count, 3)
                self.assertGreater(len(producer.public_key_der), 100)
                self.assertEqual(producer.data_count, 0)
                self.assertEqual(producer.wire_bytes, 0)
                reopened = FileSegmentedObjectProducer(
                    producer.base_name, str(payload), signing_identity="/spec164/test/publisher",
                    max_segment_size=4096)
                self.assertEqual(reopened.versioned_name, producer.versioned_name)
                self.assertEqual(reopened.public_key_der, producer.public_key_der)
                with self.assertRaises(ValueError):
                    FileSegmentedObjectProducer(producer.base_name, str(payload),
                        signing_identity="/spec164/test/publisher", digest_signing=True)
            finally:
                for key, value in previous.items():
                    if value is None: os.environ.pop(key, None)
                    else: os.environ[key] = value
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
