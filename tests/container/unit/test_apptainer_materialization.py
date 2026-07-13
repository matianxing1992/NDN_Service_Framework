from __future__ import annotations
import json,os
from pathlib import Path
import subprocess,tempfile,unittest
from contract._support import REPO
SCRIPT=REPO/"packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/materialize-sif.sh"
class ApptainerMaterializationTest(unittest.TestCase):
    def test_materialization_records_oci_and_sif_digests(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); bindir=root/"bin"; bindir.mkdir(); fake=bindir/"apptainer"
            fake.write_text('#!/bin/sh\n[ "$1" = version ] && { echo "apptainer version 1.3.3"; exit; }\n[ "$1" = build ] && { printf sif-bytes > "$2"; exit; }\nexit 2\n'); fake.chmod(0o755)
            sha="a"*64; sif=root/"image.sif"; record=root/"record.json"
            r=subprocess.run([str(SCRIPT),"--oci-reference",f"registry/x@sha256:{sha}","--sif",str(sif),"--record",str(record)],env={**os.environ,"PATH":str(bindir)+":"+os.environ["PATH"]},text=True,capture_output=True,check=False)
            self.assertEqual(r.returncode,0,r.stderr); v=json.loads(record.read_text()); self.assertEqual(v["ociDigest"],"sha256:"+sha); self.assertRegex(v["sifSha256"],r"^sha256:[a-f0-9]{64}$")
    def test_tag_only_rejected(self):
        r=subprocess.run([str(SCRIPT),"--oci-reference","registry/x:latest","--sif","/tmp/x","--record","/tmp/y"],text=True,capture_output=True,check=False)
        self.assertEqual(r.returncode,4)
