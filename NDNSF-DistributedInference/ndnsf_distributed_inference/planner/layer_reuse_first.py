"""Optional compatibility name for reuse-weighted PreSplitFirst planning.

Reuse is a subordinate cost signal inside the V3 ``PreSplitFirstStrategy``. It
does not own topology, relax one-role/one-Provider ownership, or act as the
normal application default. This class remains only for explicit callers and
historical configuration migration.
"""

from __future__ import annotations

import hashlib

from .presplit_first import PreSplitFirstStrategy


class LayerReuseFirstStrategy(PreSplitFirstStrategy):
    """Explicit compatibility policy with the corrected V3 invariants.

    The inherited scorer orders only already-feasible placements as exact
    loaded runtime, exact assembled fragment, canonical residency, then cold.
    It cannot change role ownership or device feasibility.
    """

    name = "layer-reuse-first"
    version = "3"
    placement_profile = "DI_PLACEMENT_V3"
    state_digest = "sha256:" + hashlib.sha256(
        b"ndnsf-di-layer-reuse-first-v3").hexdigest()

    def score_feasible_residency(
        self, role, provider, *, selected_device: str,
        selected_backend: str,
    ):
        """Expose the common V3 reuse tuple for audit/evidence tooling."""

        return self._v3_reuse_cost(
            role, provider, selected_device=selected_device,
            selected_backend=selected_backend)


__all__ = ["LayerReuseFirstStrategy"]
