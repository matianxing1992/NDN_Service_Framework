"""Analysis-only checks; native protocol/runtime fixtures are separate."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "invocation_timing", ROOT / "tools/ndnsf-di/analyze_invocation_timing.py")
TIMING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TIMING)


class LocalWorkPairedQualification(unittest.TestCase):
    def coverage(self):
        coverage = dict.fromkeys(("sourceCoverageComplete", "exclusiveScopesDisjoint",
                                  "ownedWorkBounded", "clocksAndThreadsValid",
                                  "observationsComplete", "matchedConditions"), True)
        coverage["sourceCoverageDigest"] = "sha256:" + "a" * 64
        return coverage

    def pairs(self, baseline=(100, 200, 300, 400, 500), upper=None):
        if upper is None:
            upper = [value * 4 // 5 for value in baseline]
        return [{"baseline": {"lowerNs": b, "upperNs": b},
                 "candidate": {"lowerNs": min(c, b // 2), "upperNs": c}}
                for b, c in zip(baseline, upper)]

    def test_paired_indices_and_exact_interval(self):
        # Proportional endpoints stay exactly 0.8 for every paired draw.
        # Independent resampling would destroy this identity.
        pairs = self.pairs()
        bounded = TIMING.qualify_local_work_pairs(pairs, self.coverage())
        self.assertEqual(bounded["status"], "PASS")
        self.assertEqual(bounded["pairedPercentile95"], [0.8, 0.8])
        for pair in pairs:
            pair["candidate"]["lowerNs"] = pair["candidate"]["upperNs"]
        self.assertEqual(TIMING.qualify_local_work_pairs(pairs, self.coverage()), bounded)

    def test_opaque_upper_cannot_improve_result(self):
        pairs = self.pairs()
        passed = TIMING.qualify_local_work_pairs(pairs, self.coverage())
        for pair in pairs:
            pair["candidate"]["upperNs"] = pair["baseline"]["lowerNs"]
        rejected = TIMING.qualify_local_work_pairs(pairs, self.coverage())
        self.assertEqual(rejected["status"], "INCONCLUSIVE")
        self.assertGreaterEqual(rejected["pairedPercentile95"][1],
                                passed["pairedPercentile95"][1])
        # Candidate lower bounds still improve by 50%; they cannot certify it.
        self.assertEqual(rejected["worstEndpointMedianRatio"], 1)

    def test_exact_threshold_rejects_sub_float_ulp_excess(self):
        baseline = (10**18,) * 5
        exact = TIMING.qualify_local_work_pairs(
            self.pairs(baseline, (850000000000000000,) * 5), self.coverage())
        above = TIMING.qualify_local_work_pairs(
            self.pairs(baseline, (850000000000000001,) * 5), self.coverage())
        self.assertEqual(exact["status"], "PASS")
        self.assertEqual(exact["pairedPercentile95Exact"], ["17/20", "17/20"])
        self.assertEqual(above["status"], "INCONCLUSIVE")
        self.assertNotEqual(above["pairedPercentile95Exact"], ["17/20", "17/20"])

    def test_ten_pairs_preserve_even_median_without_rounding(self):
        baseline = (1,) * 5 + (4,) * 5
        pairs = self.pairs(baseline, (0,) * 5 + (3,) * 5)
        result = TIMING.qualify_local_work_pairs(pairs, self.coverage())
        # Median baseline is 2.5, candidate is 1.5: exact point ratio is 3/5.
        self.assertEqual(result["pairs"], 10)
        self.assertEqual(result["worstEndpointMedianRatio"], 0.6)
        self.assertEqual(result["pairedPercentile95Exact"], ["0", "3/4"])
        self.assertEqual(result["status"], "PASS")

    def test_ratio_of_medians_not_median_of_ratios(self):
        pairs = self.pairs((1, 2, 100, 100, 100), (1, 1, 80, 100, 100))
        result = TIMING.qualify_local_work_pairs(pairs, self.coverage())
        self.assertEqual(result["worstEndpointMedianRatio"], 0.8)
        self.assertEqual(result["status"], "INCONCLUSIVE")

    def test_missing_coverage_nested_overlap_and_unbounded_workers(self):
        for key in self.coverage():
            with self.subTest(key=key):
                coverage = self.coverage()
                del coverage[key]
                result = TIMING.qualify_local_work_pairs(self.pairs(), coverage)
                self.assertEqual(result["status"], "INCONCLUSIVE")
                self.assertIn(key, result["reasons"])

    def test_invalid_bounds_and_request_count(self):
        self.assertEqual(TIMING.qualify_local_work_pairs(None, None)["status"], "INCONCLUSIVE")
        malformed = self.pairs()
        malformed[0] = None
        self.assertEqual(TIMING.qualify_local_work_pairs(malformed, self.coverage())["status"],
                         "INCONCLUSIVE")
        malformed[0] = {"baseline": None}
        self.assertEqual(TIMING.qualify_local_work_pairs(malformed, self.coverage())["status"],
                         "INCONCLUSIVE")
        for lower, upper in ((0, 100), (-1, 100), (101, 100),
                             (True, 100), (float("nan"), 100), (1, 2**64)):
            with self.subTest(lower=lower, upper=upper):
                pairs = self.pairs()
                pairs[0]["baseline"] = {"lowerNs": lower, "upperNs": upper}
                self.assertEqual(TIMING.qualify_local_work_pairs(pairs, self.coverage())["status"],
                                 "INCONCLUSIVE")
        self.assertEqual(TIMING.qualify_local_work_pairs(self.pairs()[:4],
                         self.coverage())["reasons"], ["REQUEST_PAIR_COUNT"])


class StructuredStatisticsAnalysis(unittest.TestCase):
    def records(self):
        caller = {"schema": "ndnsf-di-native-requester-statistics-v1", "type": "request",
                  "requestId": "r", "turn": 1, "completed": True, "clockDomain": "boot:ns",
                  "startedSteadyNs": 90, "requestSubmittedSteadyNs": 100,
                  "finishedSteadyNs": 1000}
        providers = []
        for role, start in (("stage0", 200), ("stage1", 400)):
            receipt = {"requestId": "r", "sessionId": "s", "providerBootId": role + "boot",
                       "role": role, "attemptEpoch": 1, "inferenceEpoch": 0,
                       "completed": True, "startedSteadyNs": start,
                       "finishedSteadyNs": start + 100}
            providers.append({"schema": "ndnsf-di-native-request-statistics-v1",
                              "type": "request", "requestId": "r", "provider": "/" + role,
                              "providerBootId": role + "boot", "role": role, "attemptEpoch": 1,
                              "clockDomain": "boot:ns", "completionAdmitted": True,
                              "hasEpochStatistics": True, "epochsExecuted": 1,
                              "runtimeMetrics": [{"inferenceEpoch": 0,
                                                  "lastBackendExecution": receipt}],
                              "workStatistics": {"complete": True, "localWorkComplete": False,
                                                 "transfers": []}})
        return [caller, *providers]

    def analyze(self, records, roles=("stage0", "stage1")):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "statistics.jsonl"
            path.write_text("\n".join(json.dumps(r) for r in records) + "\n")
            return TIMING.analyze_statistics([path], roles)["requests"][0]

    def transfer(self, point, **changes):
        return {"plannedDataName": "/producer/activation/0", "inferenceEpoch": 0,
                "point": point, "outputReadySteadyNs": 300 if point == "output-publication" else None,
                "finishedSteadyNs": 380, "admittedSteadyNs": 390,
                "tensorBytes": 10, "encodedPayloadBytes": 20, "wireBytes": None, **changes}

    def test_exact_boundary_and_deduplicated_receipts(self):
        records = self.records()
        records[1]["runtimeMetrics"] *= 2
        result = self.analyze(records)
        self.assertEqual(result["requestWallNs"], 900)
        self.assertEqual(result["inputOptionsPreparationNs"], 10)
        self.assertEqual(result["backendServiceNs"], 200)
        self.assertEqual(result["startupBeforeFirstBackendNs"], 100)
        self.assertEqual(result["wallMinusBackendServiceNs"], 700)
        self.assertFalse(result["localWorkComplete"])

    def test_missing_role_or_expected_closure_cannot_qualify(self):
        for records, roles in ((self.records()[:2], ("stage0", "stage1")),
                               (self.records(), ())):
            result = self.analyze(records, roles)
            self.assertIsNone(result["backendServiceNs"])
            self.assertIsNone(result["startupBeforeFirstBackendNs"])

    def test_missing_epoch_and_failed_request_stay_unknown(self):
        for mutate in (lambda r: r[1].update(epochsExecuted=2),
                       lambda r: r[0].update(completed=False),
                       lambda r: r[2].update(runtimeMetrics=[]),
                       lambda r: r[1].update(completionAdmitted=False)):
            records = self.records()
            mutate(records)
            result = self.analyze(records)
            self.assertIsNone(result["backendServiceNs"])
            self.assertIsNone(result["startupBeforeFirstBackendNs"])

    def test_conflicting_receipt_and_foreign_identity_are_rejected(self):
        for field, value in (("finishedSteadyNs", 350), ("role", "foreign"),
                             ("attemptEpoch", 2), ("inferenceEpoch", 9)):
            records = self.records()
            other = copy.deepcopy(records[1]["runtimeMetrics"][0])
            other["lastBackendExecution"][field] = value
            records[1]["runtimeMetrics"].append(other)
            self.assertIsNone(self.analyze(records)["backendServiceNs"])

    def test_cross_clock_and_outside_window_do_not_produce_startup(self):
        for mutate in (lambda r: r[1].update(clockDomain="other-boot"),
                       lambda r: r[0].update(requestSubmittedSteadyNs=250),
                       lambda r: r[0].update(clockDomain=None)):
            records = self.records()
            mutate(records)
            self.assertIsNone(self.analyze(records)["startupBeforeFirstBackendNs"])

    def test_two_sessions_for_same_epoch_cannot_supply_missing_epoch(self):
        records = self.records()
        other = copy.deepcopy(records[1]["runtimeMetrics"][0])
        other["lastBackendExecution"]["sessionId"] = "other-session"
        records[1]["runtimeMetrics"].append(other)
        records[1]["epochsExecuted"] = 2
        self.assertIsNone(self.analyze(records)["backendServiceNs"])

    def test_readable_table_keeps_unknown_and_nonadditive_boundary(self):
        request = self.analyze(self.records(), ())
        text = TIMING.statistics_markdown({"requests": [request]})
        self.assertIn("UNKNOWN", text)
        self.assertIn("not network time", text)
        self.assertIn("not an additive request-wall", text)

    def test_malformed_nested_shapes_preserve_unknown_request(self):
        for mutate in (lambda r: r[1].update(runtimeMetrics=[None]),
                       lambda r: r[1].update(workStatistics=[]),
                       lambda r: r[1]["workStatistics"].update(transfers=[None]),
                       lambda r: r[1]["runtimeMetrics"][0]["lastBackendExecution"].update(
                           sessionId=[])):
            records = self.records()
            mutate(records)
            result = self.analyze(records)
            self.assertIsNone(result["backendServiceNs"])
            self.assertEqual(result["requestWallNs"], 900)

    def test_malformed_extra_record_cannot_be_silently_dropped(self):
        records = self.records()
        extra = copy.deepcopy(records[1])
        extra["workStatistics"] = []
        records.append(extra)
        self.assertIsNone(self.analyze(records)["backendServiceNs"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "statistics.jsonl"
            path.write_text("\n".join(json.dumps(r) for r in self.records()) + "\n{broken\n")
            result = TIMING.analyze_statistics([path], ("stage0", "stage1"))
            self.assertTrue(result["gaps"])
            self.assertIsNone(result["requests"][0]["backendServiceNs"])

    def test_delivery_points_join_without_summing_bytes(self):
        records = self.records()
        records[1]["workStatistics"]["transfers"] = [self.transfer("output-publication")]
        records[2]["workStatistics"]["transfers"] = [self.transfer("coordinator-fetch"),
                                                    self.transfer("worker-input")]
        row = self.analyze(records)["transfers"][0]
        self.assertEqual(row["outputReadyToFetchConsumedNs"], 80)
        self.assertEqual(row["outputReadyToWorkerAdmittedNs"], 90)
        self.assertEqual(len(row["observations"]), 3)
        for mutate in (lambda r: r[1]["workStatistics"]["transfers"].append(
                            self.transfer("output-publication")),
                       lambda r: r[2].update(clockDomain="other-boot"),
                       lambda r: r[2]["workStatistics"].update(complete=False),
                       lambda r: r[1]["workStatistics"]["transfers"][0].update(
                            outputReadySteadyNs=None)):
            changed = copy.deepcopy(records)
            mutate(changed)
            row = self.analyze(changed)["transfers"][0]
            self.assertIsNone(row["outputReadyToFetchConsumedNs"])
            self.assertIsNone(row["outputReadyToWorkerAdmittedNs"])

    def test_duplicate_requester_and_provider_are_not_double_counted(self):
        for index in (0, 1):
            records = self.records()
            records.append(copy.deepcopy(records[index]))
            result = self.analyze(records)
            self.assertIsNone(result["backendServiceNs"])
            self.assertIsNone(result["startupBeforeFirstBackendNs"])


if __name__ == "__main__":
    unittest.main()
