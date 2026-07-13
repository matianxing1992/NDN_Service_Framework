from __future__ import annotations

from pathlib import Path
import sys
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "pythonWrapper"))

from ndnsf import _ndnsf  # noqa: E402
from ndnsf.service import _from_native_response, _to_native_response, ServiceResponse  # noqa: E402


class NativeServiceResponseBindingTest(unittest.TestCase):
    def test_request_id_round_trips_across_pybind_boundary(self) -> None:
        native = _to_native_response(ServiceResponse(
            status=True, payload=b"ok", error="", request_id="/request/42"))
        self.assertTrue(hasattr(native, "request_id"))
        self.assertEqual(native.request_id, "/request/42")
        restored = _from_native_response(native)
        self.assertEqual(restored.request_id, "/request/42")
        self.assertEqual(restored.payload, b"ok")

    def test_native_binding_exports_request_id(self) -> None:
        self.assertIn("request_id", dir(_ndnsf.ServiceResponse))


if __name__ == "__main__":
    unittest.main()
