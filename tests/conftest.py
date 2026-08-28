"""Repository-local Python test import closure.

The deployed and MiniNDN runners seal both the NDNSF application package and
the DistributedRepo Python wrapper.  Direct pytest collection must use the
same source closure rather than depending on a developer's editable install.
Boundary tests that deliberately use a clean environment still construct
their own subprocess environment and are unaffected by this path setup.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
for entry in (
    ROOT / "NDNSF-DistributedRepo/pythonWrapper",
    ROOT / "NDNSF-DistributedInference",
):
    value = str(entry)
    if value not in sys.path:
        sys.path.insert(0, value)
