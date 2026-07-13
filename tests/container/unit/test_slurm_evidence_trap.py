from __future__ import annotations
from pathlib import Path
import subprocess,tempfile,unittest,json
from contract._support import REPO
SCRIPT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/finalize-evidence.sh"
class SlurmEvidenceTrapTest(unittest.TestCase):
    def test_promotes_manifest_and_preserves_original_failure(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); stage=root/"stage"; stage.mkdir(); (stage/"stdout.log").write_text("measured failure\n")
            dest=root/"project/run"
            r=subprocess.run([str(SCRIPT),"--staging",str(stage),"--destination",str(dest),"--run-id","run-1","--state","FAILED","--exit-code","9"],text=True,capture_output=True,check=False)
            self.assertEqual(r.returncode,9); self.assertTrue((dest/"promotion-manifest.json").is_file()); self.assertEqual(json.loads((dest/"terminal.json").read_text())["originalExitCode"],9)
    def test_existing_destination_fails_without_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); stage=root/"stage"; stage.mkdir(); dest=root/"dest"; dest.mkdir()
            r=subprocess.run([str(SCRIPT),"--staging",str(stage),"--destination",str(dest),"--run-id","x","--state","COMPLETED","--exit-code","0"],text=True,capture_output=True,check=False)
            self.assertEqual(r.returncode,6)
