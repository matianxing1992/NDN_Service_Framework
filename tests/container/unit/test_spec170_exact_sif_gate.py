from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
_LIB_INSERTED = str(LIB) not in sys.path
if _LIB_INSERTED:
    sys.path.insert(0, str(LIB))
if str(Path(__file__).parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).parent))

import spec170_allocation_topology as topology
from test_spec170_allocation_topology import profile

# This directory contains a generic ``profile.py`` deployment helper.  Do not
# leave it at the front of the process-wide import path: later tests importing
# the standard-library ``profile`` module (e.g. through cProfile/torch) would
# otherwise resolve the deployment helper and fail during collection.
if _LIB_INSERTED:
    sys.path.remove(str(LIB))


class Spec170ExactSifGateTest(unittest.TestCase):
    def test_exact_sif_digest_is_verified(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sif = Path(directory) / "runtime.sif"
            sif.write_bytes(b"immutable-spec170-sif")
            value = profile("d0-cpu")
            value["sifPath"] = str(sif)
            value["sifSha256"] = topology.digest_file(sif)
            self.assertEqual("PASS", topology.validate_exact_sif(value)["status"])

    def test_sif_mutation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sif = Path(directory) / "runtime.sif"
            sif.write_bytes(b"immutable-spec170-sif")
            value = profile("d1-single")
            value["sifPath"] = str(sif)
            value["sifSha256"] = topology.digest_file(sif)
            sif.write_bytes(b"tampered")
            with self.assertRaisesRegex(topology.Spec170TopologyError, "SIF_TAMPERED"):
                topology.validate_exact_sif(value)

    def test_missing_exact_sif_is_not_a_pass(self) -> None:
        value = profile("d2h-hybrid")
        with self.assertRaisesRegex(topology.Spec170TopologyError, "SIF_MISSING"):
            topology.validate_exact_sif(value)


if __name__ == "__main__":
    unittest.main()
