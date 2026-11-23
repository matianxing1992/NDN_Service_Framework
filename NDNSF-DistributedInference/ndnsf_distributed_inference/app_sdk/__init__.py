"""Application-owned NDNSF-DI orchestration surfaces.

The package itself remains importable in planning/build environments where the
native ``ndnsf`` extension is intentionally absent. Runtime owners are exported
when that dependency is installed; value contracts are always available.
"""

from .contracts import *
from .contracts import __all__ as _contracts_exports
from ..conversation import *
from ..conversation import __all__ as _conversation_exports
from .status import *
from .status import __all__ as _status_exports
from ..planner.compatibility import *
from ..planner.compatibility import __all__ as _compatibility_exports
from ..sdk.loader import (
    PlacementStrategyAllowlistEntry, discover_placement_strategies,
    select_placement_strategy,
)

_exports = [
    *_contracts_exports, *_conversation_exports, *_status_exports, *_compatibility_exports,
    "PlacementStrategyAllowlistEntry", "discover_placement_strategies",
    "select_placement_strategy",
]

try:
    from .runtime_journal import *
    from .runtime_journal import __all__ as _journal_exports
    from .client import APPClient, InferenceClient, InferenceRequestHandle
    from .controller import APPController, DistributedInferenceController
    from .deployment import APPDeployment, APPDeploymentLifecycleStore
    from .engine import DistributedInferenceEngine
    from .facades import ProviderRuntimeContext
    from .placement import *
    from .placement import __all__ as _placement_exports
    from .presplit import *
    from .presplit import __all__ as _presplit_exports
    from .provider import (
        APPProvider, ProviderActionReceipt, ProviderEvidenceSigner,
        ProviderEvidenceVerifier, ProviderOfferTrustVerifier,
        ProviderReadiness,
    )
except ModuleNotFoundError as exc:
    if exc.name not in {"ndnsf", "cryptography"}:
        raise
else:
    _exports += [
        *_journal_exports, *_placement_exports, *_presplit_exports,
        "APPClient", "InferenceClient", "InferenceRequestHandle", "APPController",
        "DistributedInferenceController", "APPDeployment", "APPDeploymentLifecycleStore",
        "DistributedInferenceEngine", "ProviderRuntimeContext", "APPProvider",
        "ProviderActionReceipt", "ProviderEvidenceSigner", "ProviderEvidenceVerifier",
        "ProviderOfferTrustVerifier", "ProviderReadiness",
    ]

__all__ = list(dict.fromkeys(_exports))
