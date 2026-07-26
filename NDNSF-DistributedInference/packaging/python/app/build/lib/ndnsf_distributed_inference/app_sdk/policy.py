"""APP-owned named policy-suite configuration."""

from ..planner.defaults import DefaultOptimizationSuite


def resolve_optimization_suite(suite=None):
    return suite or DefaultOptimizationSuite()


__all__ = ["resolve_optimization_suite"]
