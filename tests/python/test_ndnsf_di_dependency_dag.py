from __future__ import annotations

from dataclasses import replace
import hashlib
import threading
import unittest

from ndnsf_distributed_inference.core.execution import (
    DependencyDrivenExecution,
    DIResultEnvelopeV2,
    InputOutputObjectManifest,
    ResultContract,
    RoleExecutionBinding,
)


def digest(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


PLAN = digest("plan")
SCHEMA = digest("tensor-schema")
SEMANTICS = digest("result-semantics")


def bindings(*roles: str):
    return {
        role: RoleExecutionBinding(
            role=role, provider=f"/provider/{role}",
            provider_boot_epoch=f"boot-epoch-{role}",
        )
        for role in roles
    }


def manifest(src: str, consumers: tuple[str, ...], *, sequence: int = 1):
    return InputOutputObjectManifest(
        object_name=f"/objects/{src}/{sequence}",
        request_id="request-1", attempt=1, plan_digest=PLAN,
        producer_role=src, producer_provider=f"/provider/{src}",
        producer_boot_epoch=f"boot-epoch-{src}", generation=3,
        consumer_roles=consumers,
        lineage_digests=(digest(f"lineage-{src}"),),
        schema_digest=SCHEMA, segment_count=2, total_bytes=16,
        payload_digest=digest(f"payload-{src}-{sequence}"),
        aead_algorithm="AES-256-GCM", key_grant_digest=digest("grant"),
        signer_key_id=f"key-{src}", signature="valid",
        captured_at_ms=100, expires_at_ms=900,
    )


class DependencyDagV2Test(unittest.TestCase):
    def make_gate(self, roles, edges, *, result_contract=None, starts=None,
                  responses=None):
        return DependencyDrivenExecution(
            request_id="request-1", attempt=1, plan_digest=PLAN,
            roles=roles, edges=edges,
            terminal_role=(result_contract.aggregator_role
                           if result_contract else roles[-1]),
            evidence_verifier=lambda value: value.signature == "valid",
            role_bindings=bindings(*roles), generation=3, deadline_ms=1000,
            result_contract=result_contract,
            start_callback=(
                (lambda role: starts.append(role))
                if starts is not None else None),
            response_callback=(
                (lambda envelope, output: responses.append(
                    (envelope.digest(), output.digest())))
                if responses is not None else None),
        )

    def test_input_before_model_and_model_before_input_start_exactly_once(self):
        starts: list[str] = []
        gate = self.make_gate(("source", "sink"), (("source", "sink"),),
                              starts=starts)
        gate.select("source", provider="/provider/source",
                    boot_epoch="boot-epoch-source", generation=3)
        gate.ready("source", provider="/provider/source",
                   boot_epoch="boot-epoch-source", generation=3, at_ms=10)
        self.assertEqual(starts, ["source"])

        gate.select("sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=3)
        self.assertTrue(gate.accept_manifest(
            manifest("source", ("sink",)), at_ms=20))
        self.assertEqual(starts, ["source"])
        gate.ready("sink", provider="/provider/sink",
                   boot_epoch="boot-epoch-sink", generation=3, at_ms=30)
        gate.ready("sink", provider="/provider/sink",
                   boot_epoch="boot-epoch-sink", generation=3, at_ms=31)
        self.assertEqual(starts, ["source", "sink"])

        reverse: list[str] = []
        gate2 = self.make_gate(("source", "sink"), (("source", "sink"),),
                               starts=reverse)
        gate2.select("sink", provider="/provider/sink",
                     boot_epoch="boot-epoch-sink", generation=3)
        gate2.ready("sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=3, at_ms=10)
        gate2.accept_manifest(manifest("source", ("sink",)), at_ms=20)
        self.assertEqual(reverse, ["sink"])

    def test_chain_starts_stage_zero_while_downstream_is_not_ready(self):
        starts: list[str] = []
        gate = self.make_gate(
            ("s0", "s1", "s2"), (("s0", "s1"), ("s1", "s2")),
            starts=starts)
        for role in ("s0", "s1", "s2"):
            gate.select(role, provider=f"/provider/{role}",
                        boot_epoch=f"boot-epoch-{role}", generation=3)
        gate.ready("s0", provider="/provider/s0",
                   boot_epoch="boot-epoch-s0", generation=3, at_ms=1)
        self.assertEqual(starts, ["s0"])
        self.assertEqual(gate.state("s1"), "WAITING")

    def test_fan_out_branches_are_independent_and_fan_in_waits_for_all(self):
        starts: list[str] = []
        gate = self.make_gate(
            ("root", "left", "right", "merge"),
            (("root", "left"), ("root", "right"),
             ("left", "merge"), ("right", "merge")),
            starts=starts)
        for role in ("root", "left", "right", "merge"):
            gate.select(role, provider=f"/provider/{role}",
                        boot_epoch=f"boot-epoch-{role}", generation=3)
            gate.ready(role, provider=f"/provider/{role}",
                       boot_epoch=f"boot-epoch-{role}",
                       generation=3, at_ms=10)
        gate.accept_manifest(manifest("root", ("left", "right")), at_ms=20)
        self.assertIn("left", starts)
        self.assertIn("right", starts)
        gate.accept_manifest(manifest("left", ("merge",)), at_ms=30)
        self.assertNotIn("merge", starts)
        gate.accept_manifest(manifest("right", ("merge",)), at_ms=31)
        self.assertEqual(starts.count("merge"), 1)

    def test_manifest_identity_substitution_and_late_events_fail_closed(self):
        gate = self.make_gate(("source", "sink"), (("source", "sink"),))
        gate.select("sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=3)
        gate.ready("sink", provider="/provider/sink",
                   boot_epoch="boot-epoch-sink", generation=3, at_ms=10)
        self.assertFalse(gate.accept_manifest(
            replace(manifest("source", ("sink",)),
                    producer_provider="/attacker"), at_ms=20))
        self.assertTrue(gate.cancel("requester cancelled", at_ms=21))
        self.assertFalse(gate.accept_manifest(
            manifest("source", ("sink",)), at_ms=22))
        self.assertFalse(gate.ready(
            "sink", provider="/provider/sink",
            boot_epoch="boot-epoch-sink", generation=3, at_ms=22))

    def test_cancel_deadline_and_last_input_race_has_one_terminal_outcome(self):
        starts: list[str] = []
        gate = self.make_gate(("source", "sink"), (("source", "sink"),),
                              starts=starts)
        gate.select("sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=3)
        gate.ready("sink", provider="/provider/sink",
                   boot_epoch="boot-epoch-sink", generation=3, at_ms=10)
        barrier = threading.Barrier(3)

        def deliver():
            barrier.wait()
            gate.accept_manifest(manifest("source", ("sink",)), at_ms=1000)

        def expire():
            barrier.wait()
            gate.expire(at_ms=1000)

        threads = [threading.Thread(target=deliver),
                   threading.Thread(target=expire)]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join()
        self.assertIn(gate.terminal_outcome, {"EXPIRED", ""})
        if gate.terminal_outcome == "EXPIRED":
            self.assertNotIn("sink", starts)
        self.assertLessEqual(starts.count("sink"), 1)

    def test_result_contract_requires_deterministic_aggregation(self):
        with self.assertRaises(ValueError):
            ResultContract(
                sink_roles=("left", "right"),
                result_schema_digest=SCHEMA,
                result_semantics_digest=SEMANTICS,
            )
        contract = ResultContract(
            sink_roles=("left", "right", "merge"),
            aggregator_role="merge",
            result_schema_digest=SCHEMA,
            result_semantics_digest=SEMANTICS,
        )
        responses = []
        gate = self.make_gate(
            ("left", "right", "merge"),
            (("left", "merge"), ("right", "merge")),
            result_contract=contract, responses=responses)
        for role in ("left", "right", "merge"):
            gate.select(role, provider=f"/provider/{role}",
                        boot_epoch=f"boot-epoch-{role}", generation=3)
            gate.ready(role, provider=f"/provider/{role}",
                       boot_epoch=f"boot-epoch-{role}",
                       generation=3, at_ms=10)
        gate.start("left", at_ms=11)
        gate.start("right", at_ms=11)
        gate.complete("left", at_ms=20)
        gate.complete("right", at_ms=20)
        gate.accept_manifest(manifest("left", ("merge",)), at_ms=30)
        gate.accept_manifest(manifest("right", ("merge",)), at_ms=31)
        gate.start("merge", at_ms=32)
        gate.complete("merge", at_ms=40)
        output = manifest("merge", ())
        envelope = DIResultEnvelopeV2(
            request_id="request-1", attempt=1, plan_digest=PLAN,
            producer_role="merge", producer_provider="/provider/merge",
            producer_boot_epoch="boot-epoch-merge", generation=3,
            result_contract_digest=contract.digest(),
            output_manifest_digest=output.digest(),
            result_schema_digest=SCHEMA,
            result_semantics_digest=SEMANTICS,
            payload_digest=output.payload_digest,
            signer_key_id="key-merge", signature="valid",
        )
        self.assertFalse(gate.accept_result(
            replace(envelope, result_semantics_digest=digest("wrong")),
            output_manifest=output, at_ms=499))
        self.assertTrue(gate.accept_result(
            envelope, output_manifest=output, at_ms=500))
        self.assertFalse(gate.accept_result(
            envelope, output_manifest=output, at_ms=501))
        self.assertFalse(gate.cancel("too late", at_ms=502))
        self.assertTrue(gate.terminal_output_accepted)
        self.assertEqual(len(responses), 1)

    def test_superseded_attempt_rejects_every_late_event(self):
        gate = self.make_gate(("source",), ())
        self.assertTrue(gate.supersede(new_attempt=2, at_ms=100))
        self.assertFalse(gate.select(
            "source", provider="/provider/source",
            boot_epoch="boot-epoch-source", generation=3))
        self.assertFalse(gate.accept_status(
            role="source", provider="/provider/source",
            boot_epoch="boot-epoch-source", generation=3,
            request_id="request-1", attempt=1, plan_digest=PLAN,
            at_ms=101))
        self.assertEqual(gate.terminal_outcome, "SUPERSEDED")


if __name__ == "__main__":
    unittest.main()
