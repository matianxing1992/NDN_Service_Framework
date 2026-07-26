#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


REPO = Path(__file__).resolve().parents[2]
RUNNER_PATH = REPO / "Experiments/NDN_SVS_Fetcher_Queue_Causality_Minindn.py"
BUILDER_PATH = REPO / "Experiments/build_svs_fetcher_queue_causality.py"
ANALYZER_PATH = REPO / "Experiments/analyze_svs_fetcher_queue_causality.py"
RSA_HELPER = REPO / \
    "Experiments/ndn-svs-pubsub-benchmark/spec135-rsa-security.hpp"


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec135ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load(RUNNER_PATH, "spec135_runner_test")

    def test_security_helper_proves_real_rsa(self):
        text = RSA_HELPER.read_text(encoding="utf-8")
        self.assertIn("RsaKeyParams(2048)", text)
        self.assertIn("signingByIdentity(identity)", text)
        self.assertIn("SignatureSha256WithRsa", text)
        self.assertNotIn("setSha256Signing", text)

    def test_builder_is_isolated_and_bounded(self):
        text = BUILDER_PATH.read_text(encoding="utf-8")
        self.assertIn("e9913c9a957a214d699ab5eb0bc99684e06573c5", text)
        self.assertIn('std::string(raw) != "10"', text)
        self.assertIn('std::string(raw) != "40"', text)
        self.assertIn(
            'changed != ["ndn-svs/fetcher.cpp", "ndn-svs/fetcher.hpp"]',
            text,
        )
        self.assertNotIn("pib-memory:spec135-peer", text)
        self.assertNotIn("git reset", text)
        self.assertNotIn("git checkout", text)

    def test_contract_has_five_plus_three_once_only_cells(self):
        subject = {
            "profiledBinary": "/tmp/binary",
            "profiledBinarySha256": "b",
            "profiledLibrary": "/tmp/library",
            "profiledLibrarySha256": "l",
            "profileConfig": {"stageCount": 81, "sampleModulus": 100},
        }
        stage_a = [
            self.runner.cell(
                f"{ordinal:02d}-rsa-sweep-{rate}", ordinal, rate, 10, 4096,
                "rsa-boundary-sweep", subject,
            )
            for ordinal, rate in enumerate(self.runner.RATES, 1)
        ]
        contract = {
            "automaticRetry": False,
            "stageA": stage_a,
            "stageBTemplates": [
                {"fetcherWindow": 40, "maxApplicationParametersSize": 4096},
                {"fetcherWindow": 10, "maxApplicationParametersSize": 7168},
                {"fetcherWindow": 40, "maxApplicationParametersSize": 7168},
            ],
        }
        self.runner.validate_contract(contract)
        self.assertEqual(len(stage_a), 5)
        self.assertEqual(len(contract["stageBTemplates"]), 3)

    def test_boundary_rule_selects_first_unstable_without_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            (campaign / "receipts").mkdir()
            (campaign / "cells").mkdir()
            stage_a = []
            for ordinal, rate in enumerate(self.runner.RATES, 1):
                cell_id = f"{ordinal:02d}-rsa-sweep-{rate}"
                config = {"cellId": cell_id, "ratePpsPerPeer": rate}
                stage_a.append(config)
                (campaign / "receipts" / f"{cell_id}.json").write_text(
                    json.dumps({"status": "COMPLETE"}), encoding="utf-8")
            contract = {
                "campaignId": "fixture",
                "stageA": stage_a,
                "boundaryRule": {
                    "attemptedScheduledMinimum": 0.98,
                    "aggregateDeliveredAttemptedMinimum": 0.98,
                    "fallbackStressRate": 1000,
                },
            }
            def fake_metrics(_campaign, config):
                ratio = 1.0 if config["ratePpsPerPeer"] < 600 else 0.95
                return {
                    "attemptedScheduledRatio": {
                        "peer-a": ratio, "peer-b": ratio},
                    "aggregateDeliveredAttemptedRatio": 1.0,
                    "rsaProof": {"peer-a": True, "peer-b": True},
                }

            with patch.object(self.runner, "measured_metrics",
                              side_effect=fake_metrics):
                result = self.runner.select_boundary(campaign, contract)
            self.assertEqual(result["selectedRatePpsPerPeer"], 600)
            self.assertEqual(result["selectionKind"],
                             "first-rsa-instability-boundary")

    def test_analyzer_declares_no_production_fix(self):
        text = ANALYZER_PATH.read_text(encoding="utf-8")
        self.assertIn("No production NDN-SVS or NDNSF change", text)
        self.assertIn("No p-values or confidence intervals", text)
        self.assertIn("RSA wire-type proof failed", text)

    def test_runner_restores_minindn_per_node_application_environment(self):
        text = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn("from minindn.util import popenGetEnv", text)
        self.assertIn('host.params["params"]["homeDir"]', text)
        self.assertIn('parameters.setdefault("cwd", _home)', text)
        self.assertIn('parameters.setdefault("env", dict(_environment))', text)


if __name__ == "__main__":
    unittest.main()
