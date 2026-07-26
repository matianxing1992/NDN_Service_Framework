from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import tempfile
import unittest

from ndnsf_distributed_inference.core import (
    BoundedExactTargetRetry, DeploymentIntent, DeploymentPlan,
    DeploymentInstance, DeploymentInstanceState, DeploymentControlJournal,
    PreparationCallbacks, ProviderAssignment,
    ProviderCapabilityOffer, ReadySetCoordinator, SelectionGatedProvider,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal


def intent() -> DeploymentIntent:
    return DeploymentIntent(
        "/user", "request-1", 1, "repo:/model", "sha256:artifact",
        ("generic-v1",), ("prefill", "decode"), 20_000,
        "repo:/input", {"memory": "bounded"})


def offer(provider: str, boot: str, roles: tuple[str, ...]):
    return ProviderCapabilityOffer(
        provider, boot, (1,), False, ("onnx",), ("cpu",), ("fp32",),
        roles, 1, 0, 25, 15_000)


def plan() -> DeploymentPlan:
    return DeploymentPlan(
        "/user", "request-1", 1, "generic-v1", ("sha256:artifact",),
        (ProviderAssignment("prefill", "/p1", "boot-1"),
         ProviderAssignment("decode", "/p2", "boot-2")),
        "distributed", 20_000, "sha256:selection",
        {"/p1": "a" * 48, "/p2": "b" * 48}, "/user/KEY/1")


class SelectionGatedCoreTest(unittest.TestCase):
    def provider(self, name, boot, roles, events, *, fail_at=""):
        def stage(label):
            def run(_plan, assignment):
                events.append((name, assignment.role, label))
                if fail_at == label:
                    raise RuntimeError(f"{label} failed")
            return run
        return SelectionGatedProvider(
            name, boot, lambda _intent: offer(name, boot, roles),
            PreparationCallbacks(stage("verify"), stage("load"), stage("warm"),
                                 lambda instance: events.append((name, instance.role, "release"))),
            activation_verifier=lambda message: message.signature == "sig:user")

    def test_request_and_ack_have_zero_deployment_side_effects(self):
        events = []
        provider = self.provider("/p1", "boot-1", ("prefill",), events)
        result = provider.acknowledge(intent(), now_ms=1_000)
        self.assertEqual(result.provider, "/p1")
        self.assertEqual(provider.counters.mutation_total(), 0)
        self.assertEqual(events, [])

    def test_only_exact_selection_prepares_in_verify_load_warm_order(self):
        events = []
        provider = self.provider("/p1", "boot-1", ("prefill",), events)
        instance = provider.select(plan(), "prefill", now_ms=1_000)
        self.assertEqual(instance.state, DeploymentInstanceState.READY)
        self.assertEqual([item[2] for item in events], ["verify", "load", "warm"])
        with self.assertRaisesRegex(ValueError, "does not bind"):
            provider.select(plan(), "decode", now_ms=1_000)
        self.assertEqual(provider.counters.execute, 0)

    def test_complete_ready_set_activates_all_and_duplicate_is_idempotent(self):
        selected = plan(); events = []
        p1 = self.provider("/p1", "boot-1", ("prefill",), events)
        p2 = self.provider("/p2", "boot-2", ("decode",), events)
        p1.select(selected, "prefill", now_ms=1_000)
        p2.select(selected, "decode", now_ms=1_000)
        coordinator = ReadySetCoordinator(selected, verifier=lambda message: message.signature.startswith("sig:"))
        first = p1.ready_message(selected, "prefill", sequence=1,
                                 expires_at_ms=10_000, signer="/p1", signature="sig:p1")
        second = p2.ready_message(selected, "decode", sequence=1,
                                  expires_at_ms=10_000, signer="/p2", signature="sig:p2")
        coordinator.accept(first, now_ms=1_000)
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            coordinator.activate(sequence=1, signature="sig:user", now_ms=1_000)
        coordinator.accept(second, now_ms=1_000)
        activation = coordinator.activate(sequence=1, signature="sig:user", now_ms=1_000)
        self.assertTrue(p1.activate(selected, "prefill", activation, now_ms=1_000))
        self.assertFalse(p1.activate(selected, "prefill", activation, now_ms=1_000))
        self.assertTrue(p2.activate(selected, "decode", activation, now_ms=1_000))
        self.assertEqual(p1.execute(selected, "prefill", lambda: b"ok"), b"ok")
        self.assertEqual(p1.counters.execute, 1)

    def test_prepare_failure_releases_and_never_executes(self):
        events = []
        provider = self.provider("/p1", "boot-1", ("prefill",), events, fail_at="warm")
        with self.assertRaisesRegex(RuntimeError, "warm failed"):
            provider.select(plan(), "prefill", now_ms=1_000)
        instance = provider.instances[(plan().digest(), "prefill")]
        self.assertEqual(instance.state, DeploymentInstanceState.FAILED)
        self.assertEqual(provider.counters.release, 1)
        self.assertEqual(provider.counters.execute, 0)

    def test_boot_epoch_and_activation_binding_fail_closed(self):
        selected = plan(); events = []
        provider = self.provider("/p1", "boot-restarted", ("prefill",), events)
        with self.assertRaisesRegex(ValueError, "boot epoch"):
            provider.select(selected, "prefill", now_ms=1_000)

    def test_exact_target_retry_is_finite_and_deadline_bounded(self):
        retry = BoundedExactTargetRetry(max_retries=2, deadline_ms=5_000)
        self.assertEqual([retry.record("/p1", now_ms=1_000) for _ in range(3)], [1, 2, 3])
        with self.assertRaisesRegex(RuntimeError, "exhausted"):
            retry.record("/p1", now_ms=1_000)
        with self.assertRaisesRegex(RuntimeError, "expired"):
            retry.record("/p2", now_ms=5_000)
        self.assertEqual((retry.total_attempts, retry.retry_attempts,
                          retry.exhausted), (3, 2, 2))

    def test_ready_ack_exhaustion_releases_prepared_instance(self):
        selected = plan(); events = []
        provider = self.provider("/p1", "boot-1", ("prefill",), events)
        provider.select(selected, "prefill", now_ms=1_000)
        first = provider.ready_message(
            selected, "prefill", sequence=1, expires_at_ms=5_000,
            signer="/p1", signature="sig:p1")
        duplicate = provider.ready_message(
            selected, "prefill", sequence=1, expires_at_ms=5_000,
            signer="/p1", signature="sig:p1")
        self.assertEqual(first.digest(), duplicate.digest())
        self.assertEqual((provider.counters.ready_notifications,
                          provider.counters.ready_duplicates), (1, 1))
        provider.expire(selected, "prefill")
        self.assertEqual(
            provider.instances[(selected.digest(), "prefill")].state,
            DeploymentInstanceState.RELEASED)
        self.assertEqual(provider.counters.release, 1)

    def test_v1_journal_import_rewrites_v2_once_and_restart_reads_canonical_state(self):
        selected = plan()
        instance = DeploymentInstance(
            "instance-1", selected.digest(), "/p1", "boot-1", "prefill",
            DeploymentInstanceState.READY, 4, selected.deadline_ms)
        with tempfile.TemporaryDirectory() as root:
            journal = RuntimeJournal.for_test(root, "provider")
            journal.append("deployment-state", {
                "deploymentId": "legacy", "revision": "sha256:legacy",
                "state": "READY"})
            store = DeploymentControlJournal(journal)
            plans, instances = store.restore(
                legacy_importer=lambda _payload: (selected, (instance,)))
            self.assertEqual(store.legacy_import_count, 1)
            self.assertIn(selected.digest(), plans)
            self.assertIn(instance.instance_id, instances)

            restarted = DeploymentControlJournal(
                RuntimeJournal.for_test(root, "provider"))
            plans2, instances2 = restarted.restore()
            self.assertEqual(restarted.legacy_import_count, 0)
            self.assertEqual(plans2[selected.digest()], selected)
            self.assertEqual(instances2[instance.instance_id], instance)


if __name__ == "__main__":
    unittest.main()
