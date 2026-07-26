from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from Experiments.NDN_SVS_V3_Interop_Minindn import (
    analyze_cell,
    aggregate_formal_summaries,
    candidate_interop_paths,
    candidate_ndn_svs_path,
    count_sync_ack_lines,
    covered_remote_sequences,
    final_state,
    normalize_node,
    require_formal_privilege,
    validate_node_bin,
)


class Spec114RunOnceContractTest(unittest.TestCase):
    def test_interop_peers_are_ndnsf_owned_cpp_and_typescript(self) -> None:
        root = REPO / "examples/interop/ndn-svs-v3"
        cpp = root / "build/svs3-peer"
        ts = root / "ndnts/svs3-peer.ts"
        manifest = {"identity": {"interop": {
            "owner": "NDNSF", "cppPeer": str(cpp),
            "ndntsSource": str(ts), "ndntsSourceLanguage": "TypeScript",
        }}}
        if cpp.is_file():
            self.assertEqual(candidate_interop_paths(manifest), (cpp.resolve(), ts.resolve()))
        self.assertTrue(ts.is_file())
        self.assertIn("function arg(name: string", ts.read_text(encoding="utf-8"))
        self.assertNotIn("tests/interop", ts.as_posix())

    def test_active_ndn_svs_branches_do_not_track_external_harness(self) -> None:
        ndn_svs = REPO.parent / "ndn-svs"
        forbidden = ("tests/interop/", "node_modules/", "package-lock.json",
                     "svs3-peer.cpp", "svs3-peer.mjs", "svs3-peer.ts")
        for ref in ("master", "Experimental"):
            tracked = subprocess.check_output(
                ["git", "ls-tree", "-r", "--name-only", ref],
                cwd=ndn_svs, text=True,
            ).splitlines()
            self.assertFalse(
                [path for path in tracked if any(token in path for token in forbidden)],
                f"{ref} tracks an external interop harness",
            )

    def test_candidate_source_path_comes_from_manifest(self) -> None:
        manifest = {"identity": {"ndnSvs": {"path": "/tmp/spec115-candidate"}}}
        self.assertEqual(candidate_ndn_svs_path(manifest), Path("/tmp/spec115-candidate"))

    def test_non_root_formal_preflight_precedes_run_once_mutation(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "through sudo"):
            require_formal_privilege(True, False, euid=1000)
        require_formal_privilege(True, False, euid=0)
        require_formal_privilege(True, True, euid=1000)

    def test_declared_formal_cells_are_unique(self) -> None:
        from Experiments.spec114_candidate_manifest import CELLS
        self.assertEqual(len(CELLS), 6)
        self.assertEqual(len(set(CELLS)), 6)

    def test_existing_cell_directory_is_not_a_runnable_cell(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cell = Path(temporary) / "loss00-run01"
            cell.mkdir()
            (cell / "summary.json").write_text(json.dumps({"complete": False}), encoding="utf-8")
            self.assertTrue(cell.exists())
            self.assertFalse(bool(json.loads((cell / "summary.json").read_text())["complete"]))

    def test_callback_ranges_expand_without_duplicate_coverage(self) -> None:
        events = [
            {"event": "update", "nodeName": "/8=ndnts", "low": 1, "high": 3},
            {"event": "update", "nodeName": "/8=ndnts", "low": 4, "high": 5},
        ]
        result = covered_remote_sequences(events, "/ndnts")
        self.assertEqual(result["covered"], [1, 2, 3, 4, 5])
        self.assertEqual(result["duplicates"], [])

    def test_analysis_requires_equal_final_vectors(self) -> None:
        cpp = [
            {"event": "update", "nodeName": "/ndnts", "low": 1, "high": 2},
            {"event": "state", "nodeName": "/cpp", "bootstrapTime": 10, "high": 2},
            {"event": "state", "nodeName": "/ndnts", "bootstrapTime": 20, "high": 2},
        ]
        ndnts = [
            {"event": "update", "nodeName": "/8=cpp", "low": 1, "high": 2},
            {"event": "state", "nodeName": "/8=cpp", "bootstrapTime": 10, "high": 2},
            {"event": "state", "nodeName": "/8=ndnts", "bootstrapTime": 20, "high": 2},
        ]
        result = analyze_cell(cpp, ndnts, 2)
        self.assertTrue(result["coveragePassed"])
        self.assertTrue(result["finalVectorsEqual"])
        self.assertEqual(result["duplicateCoverageCount"], 0)
        self.assertEqual(final_state(cpp), final_state(ndnts))
        self.assertEqual(normalize_node("/8=cpp"), "/cpp")

    def test_sync_ack_detection_ignores_interest_with_embedded_data(self) -> None:
        prefix = "/ndn/spec114/test"
        text = f"INTEREST: {prefix}/v=3 params=Data\nDATA: {prefix}/v=3\n"
        self.assertEqual(count_sync_ack_lines(text, prefix), 1)

    def test_node_runtime_is_explicit_and_modern(self) -> None:
        node = Path(os.environ.get(
            "SPEC114_NODE_BIN", "/home/tianxing/.local/node-v22.23.1/bin/node"))
        identity = validate_node_bin(node)
        self.assertEqual(identity["version"], "v22.23.1")
        self.assertEqual(Path(identity["path"]), node.resolve())
        self.assertEqual(len(identity["sha256"]), 64)

    def test_formal_aggregation_requires_all_six_bound_cells(self) -> None:
        from Experiments.spec114_candidate_manifest import CELLS
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = root / "candidate-manifest.json"
            manifest = {"candidateId": "candidate"}
            manifest_path.write_text("{}", encoding="utf-8")
            for cell_id in CELLS:
                cell = root / cell_id
                cell.mkdir()
                (cell / "summary.json").write_text(json.dumps({
                    "candidateId": "candidate", "cellId": cell_id,
                    "formal": True, "status": "SUCCESS",
                }), encoding="utf-8")
            aggregate = aggregate_formal_summaries(manifest_path, manifest)
            self.assertTrue(aggregate["passed"])
            self.assertEqual(len(aggregate["cells"]), 6)


if __name__ == "__main__":
    unittest.main()
