from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
EXAMPLES = REPO / "examples/python"


def load_script(name: str, filename: str):
    common = types.ModuleType("common")

    def add_process_arguments(parser):
        parser.add_argument("--start-local-nfd", action="store_true")

    common.add_process_arguments = add_process_arguments
    common.optional_local_nfd = lambda enabled: contextlib.nullcontext()
    common.session_kwargs = lambda args: {}
    ndnsf = types.ModuleType("ndnsf")
    ndnsf.ServiceProvider = object
    ndnsf.ServiceUser = object

    old_common = sys.modules.get("common")
    old_ndnsf = sys.modules.get("ndnsf")
    old_path = list(sys.path)
    try:
        sys.modules["common"] = common
        sys.modules["ndnsf"] = ndnsf
        sys.path.insert(0, str(EXAMPLES))
        spec = importlib.util.spec_from_file_location(name, EXAMPLES / filename)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path[:] = old_path
        if old_common is None:
            sys.modules.pop("common", None)
        else:
            sys.modules["common"] = old_common
        if old_ndnsf is None:
            sys.modules.pop("ndnsf", None)
        else:
            sys.modules["ndnsf"] = old_ndnsf


class Response:
    def __init__(self, payload: bytes, *, status: bool = True, error: str = "", request_id: str = "wire-id"):
        self.payload = payload
        self.status = status
        self.error = error
        self.request_id = request_id


class Spec112SegmentedResponseToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.provider = load_script("spec112_provider", "segmented_response_provider.py")
        cls.user = load_script("spec112_user", "segmented_response_user.py")

    def test_size_sequence_expands_repetitions_and_rejects_invalid_or_excessive_values(self) -> None:
        self.assertEqual(self.user.parse_sizes("64,8000x2,4000"), [64, 8000, 8000, 4000])
        for invalid in ("", "0", "64x0", "-1", "64x10001", "abc"):
            with self.subTest(invalid=invalid), self.assertRaises(Exception):
                self.user.parse_sizes(invalid)

    def test_request_identity_and_payload_are_exact_and_request_specific(self) -> None:
        request = self.user.encode_request("candidate-cell", 7, 6500)
        parsed = self.provider.decode_request(request, 16000)
        self.assertEqual(parsed, ("candidate-cell", 7, 6500))
        expected = self.user.expected_payload(6500, "candidate-cell", 7)
        self.assertEqual(self.provider.response_payload(6500, "candidate-cell", 7), expected)
        self.assertEqual(len(expected), 6500)
        self.assertNotEqual(expected, self.user.expected_payload(6500, "candidate-cell", 8))
        with self.assertRaises(ValueError):
            self.provider.decode_request(b"SIZE:6500:7", 16000)

    def test_user_emits_machine_readable_results_and_nonzero_exit_on_byte_mismatch(self) -> None:
        module = self.user

        class FakeUser:
            def __init__(self, **kwargs):
                self.calls = 0

            def request_service(self, service, request, **kwargs):
                run_id, index, size = module.decode_request(request)
                self.calls += 1
                payload = module.expected_payload(size, run_id, index)
                if self.calls == 2:
                    payload = payload[:-1] + b"X"
                return Response(payload)

        output = io.StringIO()
        with mock.patch.object(module, "ServiceUser", FakeUser), contextlib.redirect_stdout(output):
            exit_code = module.main([
                "--run-id", "candidate-cell",
                "--sizes", "64,4000",
                "--mode", "normal",
            ])

        self.assertEqual(exit_code, 1)
        lines = output.getvalue().splitlines()
        results = [json.loads(line.split(" ", 1)[1]) for line in lines if line.startswith("SEGMENTED_RESPONSE_RESULT ")]
        summary = json.loads([line for line in lines if line.startswith("SEGMENTED_RESPONSE_SUMMARY ")][0].split(" ", 1)[1])
        self.assertEqual([item["index"] for item in results], [0, 1])
        self.assertEqual([item["requestIdentity"] for item in results], ["candidate-cell:0", "candidate-cell:1"])
        self.assertEqual(summary["schemaVersion"], "spec112-segmented-user-v1")
        self.assertEqual(summary["runId"], "candidate-cell")
        self.assertEqual((summary["total"], summary["passed"], summary["failed"], summary["exitCode"]), (2, 1, 1, 1))

    def test_user_validates_names_timeouts_and_run_identity(self) -> None:
        parser = self.user.build_parser()
        invalid_argument_sets = [
            ["--run-id", "bad id"],
            ["--run-id", "cell", "--service", "HELLO"],
            ["--run-id", "cell", "--provider", "provider"],
            ["--run-id", "cell", "--timeout-ms", "0"],
            ["--run-id", "cell", "--ack-timeout-ms", "0"],
            ["--run-id", "cell", "--pause-after-index", "-1", "--resume-file", "/tmp/x"],
        ]
        for arguments in invalid_argument_sets:
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                parser.parse_args(arguments)

        with self.assertRaises(SystemExit):
            self.user.main(["--run-id", "cell", "--sizes", "64,64", "--pause-after-index", "0"])

    def test_provider_validates_configuration_and_rejects_oversized_request(self) -> None:
        parser = self.provider.build_parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(["--service", "HELLO"])
        with self.assertRaises(SystemExit):
            parser.parse_args(["--max-response-bytes", "0"])
        with self.assertRaises(ValueError):
            self.provider.decode_request(self.user.encode_request("cell", 0, 16001), 16000)


if __name__ == "__main__":
    unittest.main()
