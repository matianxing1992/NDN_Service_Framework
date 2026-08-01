"""Application-owned NDNSF-DI orchestration surfaces.

The package itself remains importable in planning/build environments where the
native ``ndnsf`` extension is intentionally absent. Runtime owners are exported
when that dependency is installed; value contracts are always available.
"""

from .contracts import *
from .status import *
from ..planner.compatibility import *
from ..sdk.loader import (
    PlacementStrategyAllowlistEntry, discover_placement_strategies,
    select_placement_strategy,
)

try:
    from .runtime_journal import *
    from .client import APPClient
    from .controller import APPController, DistributedInferenceController
    from .deployment import APPDeployment, APPDeploymentLifecycleStore
    from .engine import DistributedInferenceEngine
    from .execution_control import *
    from .facades import ProviderRuntimeContext
    from .placement import *
    from .presplit import *
    from .provider import (
        APPProvider, ProviderActionReceipt, ProviderEvidenceSigner,
        ProviderEvidenceVerifier, ProviderReadiness,
    )
except ModuleNotFoundError as exc:
    if exc.name not in {"ndnsf", "cryptography"}:
        raise

__all__ = [name for name in globals() if not name.startswith("_")]
