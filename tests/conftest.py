"""Select source tests or preserve the requested installed Python closure.

The deployed and MiniNDN runners seal both the NDNSF application package and
the DistributedRepo Python wrapper.  Direct pytest collection must use the
same source closure rather than depending on a developer's editable install.
With NDNSF_PREFER_INSTALLED_NATIVE=1, use the selected interpreter and its
installed packages without adding repository implementation directories.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if os.environ.get("NDNSF_PREFER_INSTALLED_NATIVE") != "1":
    for entry in (
        ROOT / "NDNSF-DistributedRepo/pythonWrapper",
        ROOT / "NDNSF-DistributedInference",
    ):
        value = str(entry)
        if value not in sys.path:
            sys.path.insert(0, value)
