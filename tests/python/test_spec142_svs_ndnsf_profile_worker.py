from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from Experiments import NDN_SVS_NDNSF_Profile_Worker_Minindn as runner
from Experiments import analyze_svs_ndnsf_profile_worker as analyzer


def valid_peer_summary(
    *,
    mode: str = "face-inline-rsa",
    rate: int = 400,
    measure: int = 60,
) -> dict[str, object]:
    scheduled = rate * measure
    return {
        "schema": runner.PEER_SUMMARY_SCHEMA,
        "runtimeProfile": runner.RUNTIME_PROFILE_NAME,
        "peer": "peer-a",
        "mode": mode,
        "ratePerPeer": rate,
        "measureSeconds": measure,
        "protocolVersion": 3,
        "syncInterestLifetimeMs": 1000,
        "syncSuppressionMs": 1,
        "periodicSyncMs": 30000,
        "useTimestamp": False,
        "applicationPayloadBytes": 256,
        "maxPiggyDataSize": 800,
        "maxApplicationParametersSize": 4096,
        "mappingFetchWindow": 10,
        "mappingFetchRetries": 0,
        "mappingFetchFailureBackoffMs": 200,
        "publicationFetchWindow": runner.adaptive_fetch_window(rate),
        "publicationFetchRetries": 2,
        "publicationFetchInnerRetries": 2,
        "publicationFetchInterestLifetimeMs": 500,
        "publicationFetchMinInterestLifetimeMs": 250,
        "publicationFetchMaxInterestLifetimeMs": 2000,
        "publicationFetchFailureBackoffMs": 50,
        "publicationFetchMaxBackoffMs": 2000,
        "parallelSyncProcessing": True,
        "parallelSyncProcessingWorkers": 4,
        "parallelSyncProcessingQueue": 256,
        "parallelSyncProduction": True,
        "parallelSyncProductionWorkers": 4,
        "parallelSyncProductionQueue": 256,
        "parallelSyncProductionSigning": False,
        "parallelSyncProductionExtraBlock": True,
        "syncInterestBatching": False,
        "syncInterestBatchWindowMs": 0,
        "publicationWorkers": 0 if mode == "face-inline-rsa" else 1,
        "publicationWorkerQueueCapacity": 4096,
        "scheduledMeasured": scheduled,
        "attemptedMeasured": scheduled,
        "acceptedMeasured": scheduled,
        "deliveredMeasured": scheduled,
        "pacerFailed": False,
        "pacerError": "",
        "faceThreadHash": 1,
        "pacerThreadHash": 2,
        "publishCallThreadHash": 1 if mode == "face-inline-rsa" else 2,
        "publishCallsOnFace": scheduled if mode == "face-inline-rsa" else 0,
        "publishCallsOnPacer": scheduled if mode == "worker-rsa" else 0,
        "dataSignatureType": 1,
        "interestSignatureType": 0,
        "syncEnvelopeSignatureType": 1,
        "syncInterestSigned": False,
        "dataValid": scheduled,
        "interestValid": 0,
        "dataInvalid": 0,
        "interestInvalid": 0,
        "invalid": 0,
        "maxActiveSigners": 1,
        "workerOutstanding": 0,
        "faceDispatchAbandoned": 0,
        "signedPublicationWireBytesCount": scheduled,
        "signedPublicationWireBytesTotal": scheduled * 700,
        "signedPublicationWireBytesMax": 700,
        "piggybackEligibleCount": scheduled,
        "piggybackIneligibleCount": 0,
        "piggybackSentCount": scheduled,
        "piggybackReceivedCount": scheduled,
        "piggybackDeliveredCount": scheduled,
        "publicationFetchFallbackCount": 0,
        "publicationFetchDispatchedAtDrainEnd": 0,
        "publicationFetchDataAtDrainEnd": 0,
        "publicationFetchRetriesAtMeasureStart": 0,
        "publicationFetchRetriesAtMeasureEnd": 0,
        "publicationFetchNacksAtMeasureStart": 0,
        "publicationFetchNacksAtMeasureEnd": 0,
        "publicationFetchTimeoutsAtMeasureStart": 0,
        "publicationFetchTimeoutsAtMeasureEnd": 0,
        "publicationFetchRetriesAtDrainEnd": 0,
        "publicationFetchNacksAtDrainEnd": 0,
        "publicationFetchTimeoutsAtDrainEnd": 0,
        "mappingFetchDispatchedAtDrainEnd": 0,
        "mappingFetchDataAtDrainEnd": 0,
        "mappingFetchRetriesAtMeasureStart": 0,
        "mappingFetchRetriesAtMeasureEnd": 0,
        "mappingFetchNacksAtMeasureStart": 0,
        "mappingFetchNacksAtMeasureEnd": 0,
        "mappingFetchTimeoutsAtMeasureStart": 0,
        "mappingFetchTimeoutsAtMeasureEnd": 0,
        "mappingFetchRetriesAtDrainEnd": 0,
        "mappingFetchNacksAtDrainEnd": 0,
        "mappingFetchTimeoutsAtDrainEnd": 0,
    }


class RuntimeProfileTests(unittest.TestCase):
    def test_contract_constants_match_effective_ndnsf_profile(self) -> None:
        self.assertEqual(runner.RUNTIME_PROFILE_NAME, "ndnsf-v3")
        self.assertEqual(runner.QUALIFICATION_MATRIX, (("face-inline-rsa", 400),
                                                       ("worker-rsa", 400)))
        self.assertEqual(
            runner.FORMAL_MATRIX,
            (
                ("face-inline-rsa", 600),
                ("worker-rsa", 600),
                ("face-inline-rsa", 800),
                ("worker-rsa", 800),
            ),
        )
        self.assertEqual(runner.TIMING, (10, 60, 10))
        self.assertEqual(runner.adaptive_fetch_window(400), 128)
        self.assertEqual(runner.adaptive_fetch_window(600), 128)
        self.assertEqual(runner.adaptive_fetch_window(800), 128)

    def test_valid_profile_allows_normal_fetch_counts_without_recovery(self) -> None:
        summary = valid_peer_summary()
        summary["publicationFetchDispatchedAtDrainEnd"] = 12
        summary["publicationFetchDataAtDrainEnd"] = 12
        summary["mappingFetchDispatchedAtDrainEnd"] = 3
        summary["mappingFetchDataAtDrainEnd"] = 3

        errors = runner.validate_peer_profile(
            summary,
            "face-inline-rsa",
            400,
            60,
            "peer-a",
        )

        self.assertEqual(errors, [])

    def test_v2_and_forced_fetch_profile_are_rejected(self) -> None:
        summary = valid_peer_summary()
        summary["protocolVersion"] = 2
        summary["maxPiggyDataSize"] = 1
        summary["publicationFetchWindow"] = 64
        summary["syncInterestBatching"] = True
        summary["syncInterestBatchWindowMs"] = 5

        errors = runner.validate_peer_profile(
            summary,
            "face-inline-rsa",
            400,
            60,
            "peer-a",
        )

        self.assertTrue(any("protocolVersion" in error for error in errors))
        self.assertTrue(any("maxPiggyDataSize" in error for error in errors))
        self.assertTrue(any("publicationFetchWindow" in error for error in errors))
        self.assertTrue(any("syncInterestBatching" in error for error in errors))

    def test_any_measurement_recovery_activation_marks_profile_invalid(self) -> None:
        for key in (
            "publicationFetchRetriesAtMeasureEnd",
            "publicationFetchTimeoutsAtMeasureEnd",
            "publicationFetchNacksAtMeasureEnd",
            "mappingFetchRetriesAtMeasureEnd",
            "mappingFetchTimeoutsAtMeasureEnd",
            "mappingFetchNacksAtMeasureEnd",
        ):
            with self.subTest(key=key):
                summary = valid_peer_summary()
                summary[key] = 1
                errors = runner.validate_peer_profile(
                    summary,
                    "face-inline-rsa",
                    400,
                    60,
                    "peer-a",
                )
                self.assertTrue(
                    any(error.startswith("PROFILE_INVALID:") for error in errors)
                )
                self.assertTrue(
                    any(key.replace("AtMeasureEnd", "DuringMeasure") in error
                        for error in errors)
                )

    def test_warmup_recovery_does_not_pollute_measurement_gate(self) -> None:
        summary = valid_peer_summary()
        for prefix in ("publicationFetch", "mappingFetch"):
            for metric in ("Retries", "Timeouts", "Nacks"):
                summary[f"{prefix}{metric}AtMeasureStart"] = 7
                summary[f"{prefix}{metric}AtMeasureEnd"] = 7
                summary[f"{prefix}{metric}AtDrainEnd"] = 9

        errors = runner.validate_peer_profile(
            summary, "face-inline-rsa", 400, 60, "peer-a"
        )

        self.assertEqual(errors, [])

    def test_worker_mode_is_the_only_allowed_profile_delta(self) -> None:
        inline = valid_peer_summary(mode="face-inline-rsa")
        worker = valid_peer_summary(mode="worker-rsa")

        differences = runner.mode_independent_profile_differences(inline, worker)

        self.assertEqual(differences, [])
        worker["syncSuppressionMs"] = 5
        self.assertEqual(
            runner.mode_independent_profile_differences(inline, worker),
            ["syncSuppressionMs: inline=1 worker=5"],
        )

    def test_v3_requires_rsa_envelope_not_v2_interest_signature(self) -> None:
        summary = valid_peer_summary()
        self.assertEqual(
            runner.validate_peer_profile(
                summary, "face-inline-rsa", 400, 60, "peer-a"
            ),
            [],
        )
        summary["interestSignatureType"] = 1
        summary["syncInterestSigned"] = True
        errors = runner.validate_peer_profile(
            summary, "face-inline-rsa", 400, 60, "peer-a"
        )
        self.assertTrue(any("V3 Sync Interest" in error for error in errors))


class StageAuthorizationTests(unittest.TestCase):
    def test_formal_stage_requires_passed_qualification_and_same_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            manifest = campaign / "runtime-profile-manifest.json"
            manifest.write_text("{}\n", encoding="utf-8")
            manifest_sha = runner.sha256(manifest)

            with self.assertRaisesRegex(RuntimeError, "qualification"):
                runner.require_formal_authorization(campaign, manifest_sha)

            verdict = {
                "schema": runner.QUALIFICATION_VERDICT_SCHEMA,
                "status": "PASS",
                "runtimeProfileManifestSha256": "wrong",
            }
            (campaign / "qualification-verdict.json").write_text(
                json.dumps(verdict) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "manifest"):
                runner.require_formal_authorization(campaign, manifest_sha)

            verdict["runtimeProfileManifestSha256"] = manifest_sha
            (campaign / "qualification-verdict.json").write_text(
                json.dumps(verdict) + "\n",
                encoding="utf-8",
            )
            runner.require_formal_authorization(campaign, manifest_sha)

    def test_terminal_classification_separates_profile_and_load(self) -> None:
        profile_invalid = runner.classify_terminal(
            error="",
            admission_errors=[
                "PROFILE_INVALID:peer-a:publicationFetchTimeoutsAtDrainEnd=1",
                "LOAD_UNSUSTAINED:peer-a:delivery-ratio=0.5",
            ],
        )
        self.assertEqual(profile_invalid["validity"], "PROFILE_INVALID")
        self.assertEqual(profile_invalid["outcome"], "LOAD_UNSUSTAINED")

        clean_overload = runner.classify_terminal(
            error="",
            admission_errors=[
                "LOAD_UNSUSTAINED:peer-a:delivery-ratio=0.5",
            ],
        )
        self.assertEqual(clean_overload["validity"], "PROFILE_VALID")
        self.assertEqual(clean_overload["outcome"], "LOAD_UNSUSTAINED")


class AnalyzerTests(unittest.TestCase):
    def test_raw_sample_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cell = Path(temporary)
            summary = valid_peer_summary()
            summary.update(
                {
                    "deliverySamples": 2,
                    "deliveryMeanNs": 999,
                    "deliveryP50Ns": 100,
                    "deliveryP95Ns": 200,
                    "deliveryP99Ns": 200,
                }
            )
            (cell / "peer-a-summary.json").write_text(
                json.dumps(summary) + "\n", encoding="utf-8"
            )
            (cell / "peer-a-delivery-latency.csv").write_text(
                "latencyNs\n100\n200\n", encoding="utf-8"
            )

            with self.assertRaisesRegex(RuntimeError, "statistics mismatch"):
                analyzer.analyze_peer(
                    cell,
                    "peer-a",
                    {"validity": "PROFILE_VALID", "outcome": "COMPLETE"},
                )

    def test_percentile_matches_benchmark_nearest_rank(self) -> None:
        values = list(range(1, 101))
        self.assertEqual(analyzer.percentile(values, 50), 50)
        self.assertEqual(analyzer.percentile(values, 95), 95)
        self.assertEqual(analyzer.percentile(values, 99), 99)


if __name__ == "__main__":
    unittest.main()
