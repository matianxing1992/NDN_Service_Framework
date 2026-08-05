from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.v3_lifecycle import V3AdmissionController  # noqa: E402
from ndnsf_distributed_inference.provider import (  # noqa: E402
    _dependency_view_from_v3_projection,
)


class Spec170AssignmentV3Test(unittest.TestCase):
    def test_v3_lifecycle_module_is_separate_from_v2_reservation_book(self):
        controller = V3AdmissionController("p", boot_epoch="boot-0001")
        self.assertEqual(controller.held_devices, ())
        self.assertFalse(hasattr(controller, "reserve"))

    def test_v3_projection_ignores_unrelated_global_edges(self):
        """Each Provider receives the complete sealed graph projection.

        The projection digest is global, while the handler view is local.  An
        unrelated edge therefore must not make a valid Provider Selection
        fail closed; only edges touching the selected role belong in its
        input/output view.
        """
        view = _dependency_view_from_v3_projection(
            "/stage/0",
            (
                {
                    "producers": ["/stage/0"],
                    "consumers": ["/stage/1"],
                    "key_scope": "tensor-0",
                    "topic_prefix": "/activation",
                    "tensors": ["hidden-0"],
                },
                {
                    "producers": ["/stage/1"],
                    "consumers": ["/stage/2"],
                    "key_scope": "tensor-1",
                    "topic_prefix": "/activation",
                    "tensors": ["hidden-1"],
                },
            ),
        )
        self.assertEqual([edge.key_scope for edge in view.outputs], ["tensor-0"])
        self.assertFalse(view.inputs)


if __name__ == "__main__":
    unittest.main()
