from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timedelta, timezone
import unittest


class OnDemandProtocolContractTest(unittest.TestCase):
    def test_multi_role_ack_offer_round_trip_is_advisory(self):
        from ndnsf_distributed_inference.api import (
            ProviderDeploymentOffer, ProviderDeploymentOffers,
        )

        offers = ProviderDeploymentOffers(
            request_id="/request/1", attempt=1, provider="/provider/a",
            observed_at=datetime.now(timezone.utc),
            expires_at=datetime.now(timezone.utc) + timedelta(seconds=10),
            offers=(
                ProviderDeploymentOffer(
                    role="prefill", availability="READY",
                    revision_digest="sha256:" + "1" * 64,
                    artifact_digests=("sha256:" + "2" * 64,),
                    adapter_identity="onnx/cuda", boot_epoch="boot-a"),
                ProviderDeploymentOffer(
                    role="decode", availability="NEEDS_PREPARATION"),
            ),
        )
        decoded = ProviderDeploymentOffers.from_wire(offers.to_wire())
        self.assertEqual(decoded, offers)
        self.assertFalse(decoded.all_roles_certified_ready)

    def test_request_timing_rejects_ambiguity_and_numeric_values(self):
        from ndnsf_distributed_inference.app_sdk.contracts import RequestTiming

        with self.assertRaises(ValueError):
            RequestTiming(timeout=timedelta(seconds=1),
                          deadline=datetime.now(timezone.utc))
        with self.assertRaises(TypeError):
            RequestTiming(timeout=1)  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            RequestTiming(deadline=datetime.now())

    def test_modified_signed_definition_fails_closed(self):
        from ndnsf_distributed_inference.app_sdk.application import (
            ApplicationDefinitionSigner,
        )
        from ndnsf_distributed_inference.api import (
            ArtifactReference, DeploymentConstraints, ModelIntent,
            OptimizationObjective, RequestContract,
        )

        signer = ApplicationDefinitionSigner.generate("/app/a")
        definition = signer.define(
            deployment_id="demo", deployment_owner="/owner/a",
            service="/LLM/Test", model_intent=ModelIntent(("m1",)),
            artifacts=(ArtifactReference("repo:/m", "sha256:" + "3" * 64, 1),),
            request_contract=RequestContract("in/v1", "out/v1"),
            objective=OptimizationObjective("latency"),
            constraints=DeploymentConstraints(minimum_providers=1),
            optimization_profile="default")
        signer.verify(definition)
        with self.assertRaises(ValueError):
            signer.verify(replace(definition, service="/LLM/Other"))


if __name__ == "__main__":
    unittest.main()
