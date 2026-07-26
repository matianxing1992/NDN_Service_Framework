"""External package: only documented NDNSF-DI public imports are used."""

from ndnsf_distributed_inference.planner import (
    AdvertisedExecutionTargetPolicy, BoundedExecutionTuningPolicy,
    BoundedRecoveryPolicy, EpochBoundCachePolicy,
    ExactOrCompatibleModelVariantPolicy, FloorPreservingAdmissionPolicy,
    LifecycleDeploymentPolicy, ScopedFifoSchedulingPolicy,
    SequentialPartitionPlanner,
)
from ndnsf_distributed_inference.planner.provider_assignment_policy import CostProviderAssignmentPolicy
from ndnsf_distributed_inference.sdk import OptimizationSuite


class FixtureRunnerAdapter:
    name = "fixture-runner"
    version = "1"

    def supports(self, target):
        return getattr(target, "device", "") in {"cpu", "cuda"}

    def create_runner(self, target, artifacts):
        return {"target": target, "artifacts": tuple(artifacts)}


class FixtureObserver:
    name = "fixture-observer"
    version = "1"

    def __init__(self):
        self.keys = []

    def observe(self, outcome, idempotency_key):
        self.keys.append(idempotency_key)


def create_suite():
    policies = {
        "model_variant": ExactOrCompatibleModelVariantPolicy(),
        "partition": SequentialPartitionPlanner(),
        "deployment": LifecycleDeploymentPolicy(),
        "provider_assignment": CostProviderAssignmentPolicy(),
        "scheduling": ScopedFifoSchedulingPolicy(),
        "admission": FloorPreservingAdmissionPolicy(),
        "execution_tuning": BoundedExecutionTuningPolicy(),
        "cache": EpochBoundCachePolicy(),
        "recovery": BoundedRecoveryPolicy(),
        "execution_target": AdvertisedExecutionTargetPolicy(),
    }
    return OptimizationSuite(
        policies, name="external-fixture", version="1.0.0",
        state_digest="sha256:" + "e" * 64)


__all__ = ["create_suite", "FixtureRunnerAdapter", "FixtureObserver"]
