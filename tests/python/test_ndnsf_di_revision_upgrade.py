from __future__ import annotations

import unittest

from ndnsf_distributed_inference.app_sdk.provider import APPProvider
from ndnsf_distributed_inference.core.ports import (
    CheckpointRecord, ExecutionTargetProposal, ProgressRecord,
)
from ndnsf_distributed_inference.sdk.adapters import RunnerAdapterRegistry


class Adapter:
    name="runner"; version="1"
    def supports(self,target): return target.device=="cpu"
    def create_runner(self,target,artifacts): return object()


class RevisionUpgradeTest(unittest.TestCase):
    def test_signed_ready_activation_drain_and_warmup_failure(self):
        registry=RunnerAdapterRegistry(); registry.register(Adapter())
        provider=APPProvider(
            "/p", registry, signer=lambda digest: "test-signature:" + digest,
            signer_key_id="/p/KEY/test")
        provider.register_agent(
            boot_epoch="boot-1", capabilities=("cpu",),
            capacity_by_role={"r": 1}, permission_ready=True)
        ready=provider.stage("rev-1",ExecutionTargetProposal("r","/p","runner","cpu"),("sha256:a",))
        self.assertTrue(ready.ready); self.assertTrue(ready.evidence_digest.startswith("sha256:"))
        self.assertTrue(ready.signature)
        provider.activate(ready); self.assertEqual(provider.active_revision,"rev-1")
        provider.report_progress(ProgressRecord("request",1,"r","decode",0))
        provider.report_checkpoint(CheckpointRecord("request",1,"sha256:c",0))
        provider.report_output(request_id="request",attempt_epoch=1,
                               output_epoch=1,result_digest="sha256:o")
        self.assertEqual(len(provider.events),3)
        provider.drain(); self.assertEqual(provider.active_revision,"")
        failed=provider.stage("rev-2",ExecutionTargetProposal("r","/p","missing","cuda"),())
        self.assertFalse(failed.ready)
        with self.assertRaises(ValueError): provider.activate(failed)


if __name__=="__main__": unittest.main()
