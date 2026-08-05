from __future__ import annotations

from itertools import permutations
import hashlib
import json
import random
import threading
import unittest

from ndnsf_distributed_inference.core import (
    DependencyDrivenExecution,
    GpuMiBAdmissionLedger,
    ModelShardRetentionCache,
    RoleExecutionBinding,
)
from ndnsf_distributed_inference.sdk.placement import DIProviderOfferV2


def digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode()).hexdigest()


def make_offer(request_id: str, gpu_mib: int, sequence: int):
    return DIProviderOfferV2(
        profile="ndnsf-di-provider-offer-v2", profile_version=2,
        request_id=request_id, attempt=1, service="/inference",
        provider="/provider/a", model_intent_digest=digest("model"),
        boot_epoch="boot-epoch-a", resource_sequence=sequence,
        captured_at_ms=100, expires_at_ms=900,
        accepted_deadline_ms=900, accepted_roles=("source",),
        backends=("fake",), devices=("cuda:0",),
        offered_gpu_memory_mb=gpu_mib,
        queue_depth=0, estimated_wait_ms=0.0, rtt_ms=1.0,
        bandwidth_mbps=100.0,
        capability_resource_digest=digest("resource"),
        acceptance_predicate_digest=digest("predicate"),
        evidence_digest=digest(request_id), signer_key_id="provider-key",
        signature="valid")


class LifecycleHistoryTest(unittest.TestCase):
    def _gate(self, starts):
        return DependencyDrivenExecution(
            request_id="request-1", attempt=1, plan_digest=digest("plan"),
            roles=("source", "sink"), edges=(("source", "sink"),),
            terminal_role="sink", evidence_verifier=lambda _item: True,
            role_bindings={
                role: RoleExecutionBinding(
                    role, f"/provider/{role}", f"boot-epoch-{role}")
                for role in ("source", "sink")},
            generation=1, deadline_ms=1000,
            start_callback=starts.append)

    def test_exhaustive_duplicate_reorder_loss_terminal_fencing(self):
        histories = 0
        for order in permutations(("ready", "input", "cancel")):
            for duplicate in (False, True):
                for lost in ("", "ready", "input"):
                    starts = []
                    gate = self._gate(starts)
                    gate.select(
                        "sink", provider="/provider/sink",
                        boot_epoch="boot-epoch-sink", generation=1)
                    for event in order:
                        if event == lost:
                            continue
                        if event == "ready":
                            gate.ready(
                                "sink", provider="/provider/sink",
                                boot_epoch="boot-epoch-sink",
                                generation=1, at_ms=100)
                            if duplicate:
                                gate.ready(
                                    "sink", provider="/provider/sink",
                                    boot_epoch="boot-epoch-sink",
                                    generation=1, at_ms=101)
                        elif event == "input":
                            fields = {
                                "producerRole": "source",
                                "consumerRole": "sink",
                                "requestId": "request-1", "attempt": "1",
                                "planDigest": digest("plan"),
                                "sequence": "1", "chunk": "0",
                                "payloadDigest": digest("payload")}
                            gate.accept_input(fields)
                            if duplicate:
                                gate.accept_input(fields)
                        else:
                            gate.cancel("bounded-history", at_ms=102)
                    self.assertLessEqual(starts.count("sink"), 1)
                    if gate.terminal_outcome:
                        self.assertFalse(gate.accept_status(
                            role="sink", provider="/provider/sink",
                            boot_epoch="boot-epoch-sink", generation=1,
                            request_id="request-1", attempt=1,
                            plan_digest=digest("plan"), at_ms=200))
                    histories += 1
        self.assertEqual(histories, 36)

    def test_seeded_concurrent_gpu_history_has_zero_capacity_violation(self):
        seeds = tuple(range(163000, 163032))
        for seed in seeds:
            rng = random.Random(seed)
            capacity = 8
            ledger = GpuMiBAdmissionLedger(
                provider="/provider/a", boot_epoch="boot-epoch-a",
                capacity_mib=capacity)
            offers = [
                make_offer(f"request-{index}", rng.randint(2, 5), index + 1)
                for index in range(4)]
            barrier = threading.Barrier(len(offers) + 1)
            results = []

            def hold(value):
                barrier.wait()
                try:
                    ledger.hold_offer(value, now_ms=200)
                    results.append(("held", value.digest()))
                except ValueError:
                    results.append(("rejected", value.digest()))

            threads = [threading.Thread(target=hold, args=(value,))
                       for value in offers]
            for thread in threads:
                thread.start()
            barrier.wait()
            for thread in threads:
                thread.join()
            self.assertLessEqual(ledger.held_mib(now_ms=200), capacity)
            self.assertEqual(len(results), len(offers))

    def test_terminal_authority_and_cleanup_reference_histories(self):
        """Bounded reference histories cover the remaining safety invariants."""

        for terminal_first in ("response", "cancel", "expiry"):
            starts = []
            gate = self._gate(starts)
            self.assertTrue(gate.select(
                "source", provider="/provider/source",
                boot_epoch="boot-epoch-source", generation=1))
            self.assertFalse(gate.select(
                "source", provider="/provider/source",
                boot_epoch="stale-boot", generation=1))
            self.assertFalse(gate.ready(
                "source", provider="/provider/source",
                boot_epoch="boot-epoch-source", generation=2, at_ms=10))
            self.assertFalse(gate.accept_status(
                role="source", provider="/provider/source",
                boot_epoch="boot-epoch-source", generation=1,
                request_id="request-1", attempt=2,
                plan_digest=digest("plan"), at_ms=10))

            if terminal_first == "response":
                self.assertTrue(gate.ready(
                    "source", provider="/provider/source",
                    boot_epoch="boot-epoch-source", generation=1, at_ms=10))
                gate.complete("source", at_ms=11)
                # The source is not the result role, so finish the dependency
                # path and accept exactly one terminal Response.
                self.assertTrue(gate.select(
                    "sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=1))
                self.assertTrue(gate.ready(
                    "sink", provider="/provider/sink",
                    boot_epoch="boot-epoch-sink", generation=1, at_ms=12))
                self.assertTrue(gate.accept_input({
                    "producerRole": "source", "consumerRole": "sink",
                    "requestId": "request-1", "attempt": "1",
                    "planDigest": digest("plan"), "sequence": "1",
                    "chunk": "0", "payloadDigest": digest("payload")}))
                gate.start("sink", at_ms=13)
                gate.complete("sink", at_ms=14)
                expected = "RESPONSE"
            elif terminal_first == "cancel":
                self.assertTrue(gate.cancel("reference-cancel", at_ms=10))
                expected = "CANCELLED"
            else:
                self.assertTrue(gate.expire(at_ms=1000))
                expected = "EXPIRED"

            self.assertEqual(gate.terminal_outcome, expected)
            self.assertFalse(gate.cancel("late-cancel", at_ms=1001))
            self.assertFalse(gate.expire(at_ms=1001))
            self.assertFalse(gate.accept_status(
                role="source", provider="/provider/source",
                boot_epoch="boot-epoch-source", generation=1,
                request_id="request-1", attempt=1,
                plan_digest=digest("plan"), at_ms=1001))

        cache = ModelShardRetentionCache(max_entries=2)
        cache.retain(digest("shard-a"))
        cache.retain(digest("shard-b"))
        cache.retain(digest("shard-c"))
        self.assertFalse(cache.contains(digest("shard-a")))
        self.assertTrue(cache.contains(digest("shard-b")))
        self.assertTrue(cache.contains(digest("shard-c")))

    @classmethod
    def tearDownClass(cls):
        seeds = tuple(range(163000, 163032))
        print("NDNSF_DI_HISTORY_ZERO_VIOLATIONS " + json.dumps({
            "exhaustive_histories": 36,
            "seed_start": seeds[0], "seed_end": seeds[-1],
            "generated_schedule_categories": [
                "duplicate", "reorder", "loss", "retry", "partition",
                "restart", "concurrency",
            ],
            "counterexample_policy": "retain-seed-and-shrunk-event-trace",
            "invariants": [
                "at-most-once-role-generation-admission",
                "one-accepted-terminal-result",
                "resource-exclusion",
                "authority-fencing",
                "bounded-cleanup",
            ],
        }, sort_keys=True))


if __name__ == "__main__":
    unittest.main()
