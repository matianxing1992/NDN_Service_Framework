from __future__ import annotations
import sys,tempfile,unittest
from pathlib import Path
import tarfile
from _support import TOOLS
sys.path.insert(0,str(TOOLS))
import scan_spec109_evidence as scan
class RedactionTest(unittest.TestCase):
    def test_secret_and_unrestricted_payload_scanner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/"ok.json").write_text('{"promptDigest":"sha256:abc"}')
            self.assertEqual(scan.scan_paths([root])["status"],"PASS")
            (root/"bad.log").write_text('Authorization: Bearer secret123\n"prompt":"raw text"')
            report=scan.scan_paths([root]);self.assertEqual(report["status"],"FAIL")
            self.assertEqual({x["kind"] for x in report["findings"]},{"bearer-token","raw-prompt-field"})
    def test_model_weights_are_not_read_as_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"model.safetensors";path.write_bytes(b'hf_' + b'x'*30)
            report=scan.scan_paths([path]);self.assertEqual(report["status"],"PASS");self.assertEqual(report["skippedBinaryOrLargeFiles"],1)
    def test_source_tar_is_scanned_by_member_and_synthetic_fixture_is_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); source=root/"source"; source.mkdir()
            (source/"safe.txt").write_text("digest=sha256:abc")
            fixture=source/"tests/container/itiger-qwen/unit/test_redaction.py"
            fixture.parent.mkdir(parents=True)
            fixture.write_text('Authorization: Bearer synthetic-test-only')
            archive=root/"source.tar"
            with tarfile.open(archive,"w") as output:
                output.add(source/"safe.txt",arcname="safe.txt")
                output.add(fixture,arcname="tests/container/itiger-qwen/unit/test_redaction.py")
            report=scan.scan_paths([archive])
            self.assertEqual(report["status"],"PASS")
            self.assertEqual(report["scannedArchives"],1)
            self.assertEqual(report["skippedSyntheticFixtures"],1)
            self.assertEqual(report["scannedFiles"],1)
if __name__=="__main__":unittest.main()
