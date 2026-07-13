from __future__ import annotations

import unittest

from contract._support import LIB
from adapters.base import Adapter, REQUIRED_OPERATIONS, adapter_missing_operations


class CompleteAdapter(Adapter):
    def preflight(self, profile): return {}
    def materialize(self, profile): return {}
    def start(self, profile): return {}
    def status(self, reference): return {}
    def logs(self, reference): return {}
    def evidence(self, reference): return {}
    def stop(self, reference): return {}


class AdapterInterfaceTest(unittest.TestCase):
    def test_complete_adapter_conforms(self) -> None:
        self.assertEqual(adapter_missing_operations(CompleteAdapter), [])

    def test_contract_has_required_lifecycle_operations(self) -> None:
        self.assertEqual(REQUIRED_OPERATIONS, ("preflight", "materialize", "start", "status", "logs", "evidence", "stop"))


if __name__ == "__main__":
    unittest.main()
