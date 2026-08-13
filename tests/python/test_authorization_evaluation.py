import json
import hashlib
import os
from pathlib import Path
import subprocess

import jsonschema
import yaml

from Experiments.analyze_authorization_evaluation import (
    summarize_overhead,
    summarize_minindn,
    summarize_onboarding,
    summarize_correctness,
    summarize_publication_audit,
    validate_manifest,
)
from Experiments.run_authorization_evaluation import (
    parse_auth_markers,
    parse_crypto_counter_marker,
    parse_publication_audit_markers,
    parse_scale_markers,
    parse_onboarding_marker,
    resolve_unit_tests_binary,
    validate_case_results,
)


ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "specs/172-data-centric-authorization-evaluation"


def test_auth_marker_parser_and_case_validation_are_fail_closed():
    output = "\n".join(
        [
            "noise",
            "NDNSF_AUTH_CASE case_id=allowed terminal=allow observed_executions=1 gate=response_acceptance",
            "NDNSF_AUTH_CASE case_id=denied terminal=deny observed_executions=0 gate=user_authorization\x1b[0;39;49m",
        ]
    )
    observed = parse_auth_markers(output)
    cases = [
        {
            "case_id": "allowed",
            "expected_terminal": "allow",
            "expected_executions": 1,
            "expected_gate": "response_acceptance",
        },
        {
            "case_id": "denied",
            "expected_terminal": "deny",
            "expected_executions": 0,
            "expected_gate": "user_authorization",
        },
    ]

    assert validate_case_results(cases, observed) == []
    assert validate_case_results(cases, {"allowed": observed["allowed"]}) == [
        "missing marker for denied"
    ]


def test_correctness_summary_requires_every_case_and_zero_denied_execution():
    cases = [
        {"case_id": "allowed", "expected_terminal": "allow", "expected_executions": 1},
        {"case_id": "denied", "expected_terminal": "deny", "expected_executions": 0},
    ]
    results = [
        {"case_id": "allowed", "terminal": "allow", "observed_executions": 1},
        {"case_id": "denied", "terminal": "deny", "observed_executions": 0},
    ]

    assert summarize_correctness(results, cases)["supported"] is True
    results[1]["observed_executions"] = 1
    assert summarize_correctness(results, cases)["supported"] is False


def test_onboarding_marker_and_summary_require_refresh_without_provider_changes():
    marker = parse_onboarding_marker(
        "NDNSF_ONBOARDING_CASE stale_terminal=deny refreshed_terminal=allow "
        "stale_executions=0 refreshed_executions=1 old_epoch=1 new_epoch=2 "
        "provider_manual_changes=0 refresh_operations=1 control_bytes=90 "
        "time_to_first_success_us=209"
    )
    assert marker["control_bytes"] == 90
    observations = [
        {
            **marker,
            "repetition": repetition,
            "provider_local_hashes": {
                "executable": "same",
                "service_config": "same",
                "identity": "same",
                "trust_config": "same",
            },
        }
        for repetition in range(1, 4)
    ]

    summary = summarize_onboarding(observations, expected_repetitions=3)
    assert summary["supported"] is True
    observations[2]["provider_manual_changes"] = 1
    assert summarize_onboarding(observations, expected_repetitions=3)["supported"] is False


def test_crypto_counter_parser_and_overhead_summary_keep_cold_warm_separate():
    counters = parse_crypto_counter_marker(
        "NDNSF_AUTH_OVERHEAD_COUNTERS role=user hybrid_key_epochs=4 "
        "abe_wraps=4 abe_unwraps=4 symmetric_encrypts=20 "
        "symmetric_decrypts=20 key_cache_hits=16 key_cache_misses=4 "
        "decrypt_failures=0"
    )
    assert counters["abe_wraps"] == 4
    observations = [
        {
            "repetition": 1,
            "cold_latency_ms": 12.0,
            "warm_latencies_ms": [2.0, 3.0, 4.0],
            "failures": 0,
            "crypto_counters": counters,
            "protected_content_bytes": 1000,
            "provisioning_scale": [
                {
                    "users": users,
                    "providers": providers,
                    "policy_terms": providers,
                    "encryptions": users,
                    "decryptions": users,
                    "response_bytes": providers * 10,
                    "encrypted_bytes": providers * 20,
                    "total_us": users * providers,
                }
                for users in (1, 10, 100)
                for providers in (1, 4, 16)
            ],
        }
    ]
    summary = summarize_overhead(observations, expected_repetitions=1)
    assert summary["supported"] is True
    assert summary["cold_latency_ms"]["median"] == 12.0
    assert summary["warm_latency_ms"]["p95"] >= 3.0
    assert summary["warm_run_median_ms"]["median"] == 3.0
    assert summary["per_repetition"][0]["warm_requests"] == 3


def test_scale_marker_parser_retains_registered_dimensions_and_costs():
    rows = parse_scale_markers(
        "NDNSF_AUTH_SCALE users=10 providers=4 policy_terms=4 "
        "encryptions=10 decryptions=10 response_bytes=238 "
        "encrypted_bytes=613 total_us=5713"
    )
    assert rows == [
        {
            "users": 10,
            "providers": 4,
            "policy_terms": 4,
            "encryptions": 10,
            "decryptions": 10,
            "response_bytes": 238,
            "encrypted_bytes": 613,
            "total_us": 5713,
        }
    ]


def test_minindn_summary_requires_pre_denial_post_success_and_stable_provider_hashes():
    provider_hashes = {
        "executable": "same",
        "service_config": "same",
        "identity": "same",
        "trust_config": "same",
    }
    observations = [
        {
            "case_id": "pre_onboarding_no_user",
            "terminal": "deny",
            "terminal_gate": "abe_key_readiness",
            "provider_executions": 0,
            "request_send_attempted": False,
            "policy_epoch": 1,
            "provider_local_hashes": provider_hashes,
        },
        {
            "case_id": "post_onboarding_authorized",
            "terminal": "allow",
            "terminal_gate": "response_acceptance",
            "provider_executions": 1,
            "request_send_attempted": True,
            "policy_epoch": 2,
            "provider_local_hashes": provider_hashes,
        },
    ]
    assert summarize_minindn(observations)["supported"] is True
    observations[1]["provider_executions"] = 0
    assert summarize_minindn(observations)["supported"] is False


def test_publication_audit_requires_one_validated_producer_owned_packet_per_message():
    requester = "/ndnsf/user"
    provider = "/ndnsf/provider"
    request_id = "/request-1"
    service = "/HELLO"
    rows = []
    for message_type, role, producer, packet_name in (
        (
            "REQUEST",
            "provider",
            requester,
            "/ndnsf/user/NDNSF/REQUEST/HELLO/request-1",
        ),
        (
            "ACK",
            "user",
            provider,
            "/ndnsf/provider/NDNSF/ACK/%2Fndnsf%2Fuser/HELLO/request-1",
        ),
        (
            "SELECTION",
            "provider",
            requester,
            "/ndnsf/user/NDNSF/SELECTION/%2Fndnsf%2Fprovider/HELLO/request-1",
        ),
        (
            "RESPONSE",
            "user",
            provider,
            "/ndnsf/provider/NDNSF/RESPONSE/%2Fndnsf%2Fuser/HELLO/request-1",
        ),
    ):
        rows.append(
            {
                "role": role,
                "message_type": message_type,
                "validated": True,
                "packet_present": True,
                "packet_name": packet_name,
                "producer_prefix": f"{producer}/svs/7",
                "seq_no": len(rows) + 1,
                "signer_key_locator": f"{producer}/KEY/key-id",
                "wire_digest": f"sha256:{message_type.lower()}",
                "request_id": request_id,
                "service_name": service,
                "requester_name": requester,
                "provider_name": provider,
            }
        )

    assert summarize_publication_audit(rows)["supported"] is True

    repeated_delivery = [*rows, dict(rows[1])]
    repeated_summary = summarize_publication_audit(repeated_delivery)
    assert repeated_summary["supported"] is True
    assert repeated_summary["duplicate_observations"] == 1

    signer_mismatch = [dict(row) for row in rows]
    signer_mismatch[2]["signer_key_locator"] = "/attacker/KEY/key-id"
    assert summarize_publication_audit(signer_mismatch)["supported"] is False

    missing_response = rows[:-1]
    assert summarize_publication_audit(missing_response)["supported"] is False


def test_publication_audit_marker_parser_retains_validated_packet_identity_fields():
    marker = (
        "[INFO] NDNSF_PUBLICATION_AUDIT role=provider type=REQUEST "
        "validated=true packetPresent=true "
        "packetName=/ndnsf/user/NDNSF/REQUEST/HELLO/request-1 "
        "producerPrefix=/ndnsf/user/svs/7 seqNo=19 "
        "signerKeyLocator=/ndnsf/user/KEY/key-id wireDigest=sha256:abcd "
        "requestId=/request-1 serviceName=/HELLO "
        "requesterName=/ndnsf/user providerName=/ndnsf/provider"
    )

    assert parse_publication_audit_markers(marker) == [
        {
            "role": "provider",
            "message_type": "REQUEST",
            "validated": True,
            "packet_present": True,
            "packet_name": "/ndnsf/user/NDNSF/REQUEST/HELLO/request-1",
            "producer_prefix": "/ndnsf/user/svs/7",
            "seq_no": 19,
            "signer_key_locator": "/ndnsf/user/KEY/key-id",
            "wire_digest": "sha256:abcd",
            "request_id": "/request-1",
            "service_name": "/HELLO",
            "requester_name": "/ndnsf/user",
            "provider_name": "/ndnsf/provider",
        }
    ]


def test_active_unit_test_binary_resolution_prefers_explicit_path(tmp_path):
    explicit = tmp_path / "unit-tests"
    explicit.write_text("fixture", encoding="utf-8")
    assert resolve_unit_tests_binary(ROOT, explicit) == explicit


def _authorization_cases():
    registry = yaml.safe_load(
        (FEATURE / "contracts/authorization-cases.yaml").read_text(encoding="utf-8")
    )
    return registry["cases"]


def test_case_registry_covers_both_transaction_token_replays():
    cases = {case["case_id"]: case for case in _authorization_cases()}

    assert cases["user_token_replay"]["expected_gate"] == "user_token_replay"
    assert cases["user_token_replay"]["expected_terminal"] == "deny"
    assert cases["user_token_replay"]["expected_executions"] == 0
    assert cases["provider_token_replay"]["expected_gate"] == "provider_token_replay"
    assert cases["provider_token_replay"]["expected_terminal"] == "deny"
    assert cases["provider_token_replay"]["expected_executions"] == 0


def test_every_authorization_case_has_an_unambiguous_expected_outcome():
    cases = _authorization_cases()
    required = {
        "case_id",
        "user_permission",
        "provider_permission",
        "policy_epoch",
        "signer_name_relation",
        "user_token",
        "provider_token",
        "expected_gate",
        "expected_terminal",
        "expected_executions",
    }

    assert len({case["case_id"] for case in cases}) == len(cases)
    for case in cases:
        assert required <= case.keys()
        assert case["expected_terminal"] in {"allow", "deny"}
        assert case["expected_executions"] == (
            1 if case["expected_terminal"] == "allow" else 0
        )


def test_every_registered_gate_has_a_runtime_boundary_and_exact_test_selector():
    gates = json.loads(
        (FEATURE / "contracts/runtime-gates.json").read_text(encoding="utf-8")
    )["gates"]

    for case in _authorization_cases():
        gate = gates[case["expected_gate"]]
        assert (ROOT / gate["source_path"]).is_file()
        assert gate["source_symbol"]
        assert gate["boost_test_selector"].count("/") >= 1


def test_registered_boost_test_selectors_are_executable():
    contract = json.loads(
        (FEATURE / "contracts/runtime-gates.json").read_text(encoding="utf-8")
    )
    selectors = sorted(
        {gate["boost_test_selector"] for gate in contract["gates"].values()}
    )

    binary = resolve_unit_tests_binary(ROOT)
    env = os.environ.copy()
    runtime_paths = [
        str(binary.parent),
        str(ROOT / "build/deps/ndn-svs-experimental/lib"),
    ]
    if env.get("LD_LIBRARY_PATH"):
        runtime_paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(runtime_paths)

    for selector in selectors:
        result = subprocess.run(
            [
                str(binary),
                f"--run_test={selector}",
                "--log_level=nothing",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=env,
        )
        assert result.returncode == 0, (
            f"Boost.Test selector failed: {selector}\n{result.stdout}\n{result.stderr}"
        )


def test_baseline_source_hash_inventory_matches_current_subject():
    contract = json.loads(
        (FEATURE / "contracts/runtime-gates.json").read_text(encoding="utf-8")
    )

    for relative_path, expected in contract["baseline_source_hashes"].items():
        observed = hashlib.sha256((ROOT / relative_path).read_bytes()).hexdigest()
        assert observed == expected, f"baseline source drift: {relative_path}"


def test_manifest_schema_accepts_a_minimal_terminal_run():
    schema = json.loads(
        (FEATURE / "contracts/experiment-manifest.schema.json").read_text(
            encoding="utf-8"
        )
    )
    manifest = {
        "schema_version": 1,
        "manifest_id": "spec172-contract-test",
        "subject": "authorization-contract",
        "command": ["pytest", "tests/python/test_authorization_evaluation.py"],
        "configuration": {},
        "source_hashes": {},
        "started_at": "2026-08-11T00:00:00Z",
        "completed_at": "2026-08-11T00:00:01Z",
        "terminal_status": "success",
        "artifacts": [],
    }

    jsonschema.Draft202012Validator.check_schema(schema)
    jsonschema.validate(manifest, schema)


def test_manifest_validation_checks_terminal_state_source_and_artifact_hashes(tmp_path):
    artifact = tmp_path / "evidence.json"
    artifact.write_text('{"supported": true}\n', encoding="utf-8")
    source = tmp_path / "subject.cpp"
    source.write_text("int subject = 1;\n", encoding="utf-8")
    manifest = {
        "schema_version": 1,
        "manifest_id": "spec172-validation-test",
        "subject": "authorization-contract",
        "command": ["pytest"],
        "configuration": {},
        "source_hashes": {
            str(source): hashlib.sha256(source.read_bytes()).hexdigest(),
        },
        "started_at": "2026-08-11T00:00:00Z",
        "completed_at": "2026-08-11T00:00:01Z",
        "terminal_status": "success",
        "artifacts": [
            {
                "path": artifact.name,
                "bytes": artifact.stat().st_size,
                "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            }
        ],
    }
    (tmp_path / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )

    assert validate_manifest(tmp_path) == []

    artifact.write_text("corrupted\n", encoding="utf-8")
    errors = validate_manifest(tmp_path)
    assert "artifact size mismatch: evidence.json" in errors
    assert "artifact hash mismatch: evidence.json" in errors
