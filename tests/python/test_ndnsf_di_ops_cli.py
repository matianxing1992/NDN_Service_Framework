from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import tempfile
import time
import unittest

from ndnsf_distributed_inference.app_sdk import (
    DeploymentRevision, ProviderActionReceipt, ProviderEvidenceSigner,
    ProviderReadiness,
)
from ndnsf_distributed_inference.ops.cli import definition_from_json, main


class OpsCliTest(unittest.TestCase):
    def test_validate_resolve_apply_status_delegate_to_app(self):
        with tempfile.TemporaryDirectory(dir=Path.home()) as root:
            definition=Path(root)/"deployment.json"
            definition.write_text(json.dumps({"deploymentId":"d","modelId":"qwen","roles":["r"],"artifacts":[{"uri":"file:///m","digest":"sha256:"+"a"*64,"size_bytes":1}]}))
            resolved = DeploymentRevision.resolve(definition_from_json(definition))
            signer = ProviderEvidenceSigner.generate()
            now_ms = int(time.time() * 1000)
            readiness = ProviderReadiness.issue(
                signer=signer, provider="/provider", role="r",
                revision=resolved.revision, boot_epoch="boot-1",
                artifact_digests=("sha256:" + "a" * 64,),
                adapter_name="runner", adapter_version="1", capacity=1,
                permission_ready=True, observed_at_ms=now_ms,
                expires_at_ms=now_ms + 30_000, ready=True,
                signer_key_id=signer.key_id, reason="")
            activation = ProviderActionReceipt.issue(
                signer=signer, provider="/provider", role="r",
                revision=resolved.revision, boot_epoch="boot-1",
                action="ACTIVATE", state="ACTIVE", observed_at_ms=now_ms,
                expires_at_ms=now_ms + 30_000,
                signer_key_id=signer.key_id, reason="")
            evidence = Path(root) / "evidence.json"
            evidence.write_text(json.dumps({
                "schema": "ndnsf-di-provider-evidence-bundle-v1",
                "trustedProviderKeys": {
                    signer.key_id: signer.public_pem().decode("ascii")},
                "readiness": [readiness.to_dict()],
                "actionReceipts": [activation.to_dict()],
            }))
            for command in ("validate","resolve","apply"):
                output=io.StringIO()
                argv = ["--state-root",root,"--identity","op",command,str(definition)]
                if command == "apply":
                    argv.extend(["--evidence", str(evidence)])
                with contextlib.redirect_stdout(output):
                    self.assertEqual(main(argv),0)
                self.assertTrue(output.getvalue().strip())


if __name__=="__main__": unittest.main()
