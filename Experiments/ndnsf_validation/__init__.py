"""Fail-closed validation primitives for NDNSF deployment gates.

Import concrete submodules explicitly. Keeping package initialization free of
runtime dependencies allows model-preparation containers to use the workload
contract without importing the host NDNSF binding.
"""

__all__ = []
