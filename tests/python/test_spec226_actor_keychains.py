import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
PREPARER = ROOT / "tests/minindn/prepare_spec226_keychain.sh"
TRUST_SCHEMA = ROOT / "tests/minindn/spec226-trust-schema.conf"
Q1_RUNNER = ROOT / "tests/minindn/run_spec226_q1.py"
ROOT_IDENTITY = "/example/uav"
ACTOR_IDENTITIES = (
    "/example/uav/controller",
    "/example/uav/drone/UAV1",
    "/example/uav/drone/UAV2",
    "/example/uav/drone/UAV3",
    "/example/uav/source-adapter/UAV1",
    "/example/uav/source-adapter/UAV2",
    "/example/uav/source-adapter/UAV3",
    "/example/uav/compute",
    "/example/uav/compute/display",
    "/example/uav/gs",
)


@unittest.skipUnless(shutil.which("ndnsec"), "ndnsec is required")
class Spec226ActorKeychainTests(unittest.TestCase):
    def test_q1_runner_rejects_symlink_run_root_before_touching_target(self):
        with tempfile.TemporaryDirectory(prefix="spec226-q1-symlink-") as temporary:
            root = Path(temporary)
            target = root / "target"
            target.mkdir()
            alias = root / "alias"
            alias.symlink_to(target, target_is_directory=True)

            spec = importlib.util.spec_from_file_location("run_spec226_q1", Q1_RUNNER)
            self.assertIsNotNone(spec)
            self.assertIsNotNone(spec.loader)
            runner = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = runner
            spec.loader.exec_module(runner)

            with self.assertRaisesRegex(RuntimeError, "non-symlink"):
                runner.run(SimpleNamespace(run_root=alias))
            self.assertEqual(list(target.iterdir()), [])

    def test_actors_receive_distinct_private_pib_and_tpm_homes(self):
        with tempfile.TemporaryDirectory(prefix="spec226-actor-keychains-") as temporary:
            run_root = Path(temporary) / "run"
            run_root.mkdir()
            result = subprocess.run(
                ["bash", str(PREPARER), str(run_root), str(TRUST_SCHEMA)],
                check=False,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            private = run_root / "private"
            self.assertFalse((private / "keychain").exists())
            actor_paths = []
            key_inodes = set()
            key_digests = set()
            for identity in ACTOR_IDENTITIES:
                with self.subTest(identity=identity):
                    slug = identity[len("/example/uav/"):].replace("/", "-")
                    keychain = private / "identities" / slug / "keychain"
                    pib = keychain / "pib"
                    tpm = keychain / "tpm"
                    self.assertTrue(pib.is_dir())
                    self.assertTrue(tpm.is_dir())
                    self.assertFalse(keychain.is_symlink())
                    actor_paths.append(keychain.resolve())

                    private_keys = [path for path in tpm.rglob("*") if path.is_file()]
                    self.assertEqual(len(private_keys), 1)
                    key_stat = private_keys[0].stat()
                    inode = (key_stat.st_dev, key_stat.st_ino)
                    digest = hashlib.sha256(private_keys[0].read_bytes()).hexdigest()
                    self.assertNotIn(inode, key_inodes)
                    self.assertNotIn(digest, key_digests)
                    key_inodes.add(inode)
                    key_digests.add(digest)

                    readable = private / "ca" / "issued" / f"{slug}-cert-readable.txt"
                    self.assertTrue(readable.is_file())
                    self.assertIn("/root/", readable.read_text(encoding="utf-8"))

                    environment = os.environ.copy()
                    environment.update({
                        "HOME": str(keychain / "home"),
                        "XDG_CONFIG_HOME": str(keychain / "config"),
                        "XDG_DATA_HOME": str(keychain / "data"),
                        "NDN_CLIENT_PIB": f"pib-sqlite3:{pib}",
                        "NDN_CLIENT_TPM": f"tpm-file:{tpm}",
                    })
                    cert = subprocess.run(
                        ["ndnsec", "get-default", "-c", "-i", identity],
                        check=True,
                        capture_output=True,
                        text=True,
                        env=environment,
                        timeout=10,
                    ).stdout.strip()
                    self.assertTrue(cert.startswith(identity + "/KEY/"))
                    self.assertIn("/root/", cert)

                    default_identity = subprocess.run(
                        ["ndnsec", "get-default"],
                        check=True,
                        capture_output=True,
                        text=True,
                        env=environment,
                        timeout=10,
                    ).stdout.strip()
                    self.assertEqual(default_identity, identity)

                    for trusted_identity in (ROOT_IDENTITY,) + ACTOR_IDENTITIES:
                        with self.subTest(identity=identity,
                                          trusted_identity=trusted_identity):
                            trusted_cert = subprocess.run(
                                ["ndnsec", "get-default", "-c", "-i",
                                 trusted_identity],
                                check=True,
                                capture_output=True,
                                text=True,
                                env=environment,
                                timeout=10,
                            ).stdout.strip()
                            self.assertTrue(
                                trusted_cert.startswith(trusted_identity + "/KEY/"))
                            self.assertIn(
                                "/self/" if trusted_identity == ROOT_IDENTITY else "/root/",
                                trusted_cert)

            self.assertEqual(len(set(actor_paths)), len(ACTOR_IDENTITIES))
            self.assertTrue((private / "ca" / "tpm").is_dir())
            self.assertTrue((private / "config" / "root.cert").is_file())


if __name__ == "__main__":
    unittest.main()
