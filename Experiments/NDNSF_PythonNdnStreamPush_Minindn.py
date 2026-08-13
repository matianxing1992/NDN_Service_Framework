#!/usr/bin/env python3
"""Run the real python-ndn -> StreamPublisher.push interoperability test.

The launcher gives the C++ controller and the Python Provider the same
temporary PIB/TPM, starts both against a MiniNDN NFD, and then runs the opt-in
test that signs the exact predictive Data name with python-ndn before calling
the native StreamPublisher.push path.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import sys
import time


REPO = Path(__file__).resolve().parents[1]


def stop_process(process, grace: float = 3.0) -> int | None:
    if process is None:
        return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except Exception:
            process.kill()
            process.wait(timeout=grace)
    return process.returncode


def run(output: Path) -> dict:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn
    from ndn.security import KeychainSqlite3

    output.mkdir(parents=True, exist_ok=True)
    topology = output / "topology.conf"
    topology.write_text(
        "[nodes]\nprovider:\nconsumer:\n\n[links]\n"
        "provider:consumer delay=10ms bw=100 loss=0\n",
        encoding="utf-8",
    )
    # ndn-cxx's pib-sqlite3 backend treats the configured location as a
    # directory (the SQLite file is created beneath it); python-ndn accepts
    # the same directory path for its KeychainSqlite3 initializer.
    pib = output / "pib"
    pib_db = pib / "pib.db"
    tpm = output / "tpm"
    tpm.mkdir(parents=True, exist_ok=True)
    KeychainSqlite3.initialize(str(pib_db), "tpm-file", str(tpm))

    ndn = None
    controller_process = None
    test_process = None
    error = ""
    test_rc = None
    controller_rc = None
    started = time.monotonic()
    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(output / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        provider = ndn.net["provider"]
        transport = "unix:///run/nfd/provider.sock"
        socket = Path("/run/nfd/provider.sock")
        deadline = time.monotonic() + 10
        while not socket.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        if not socket.exists():
            raise RuntimeError(f"NFD socket not ready: {socket}")

        pythonpath = os.environ.get(
            "PYTHONPATH",
            f"{REPO / 'pythonWrapper'}:/home/tianxing/.local/lib/python3.8/site-packages",
        )
        common = (
            f"cd {shlex.quote(str(REPO))} && "
            f"PYTHONPATH={shlex.quote(pythonpath)} "
            f"LD_LIBRARY_PATH={shlex.quote(str(REPO / 'build'))}:/usr/local/lib "
        )
        env = (
            f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} "
            f"NDN_CLIENT_PIB={shlex.quote('pib-sqlite3:' + str(pib))} "
            f"NDN_CLIENT_TPM={shlex.quote('tpm-file:' + str(tpm))} "
            # default_keychain() appends pib.db to this directory locator.
            f"NDNSF_PYTHON_NDN_PIB={shlex.quote('pib-sqlite3:' + str(pib))} "
            # ndn-cxx stores TPM files below ndnsec-key-file; python-ndn's
            # TpmFile points directly at that leaf directory.
            f"NDNSF_PYTHON_NDN_TPM={shlex.quote('tpm-file:' + str(tpm / 'ndnsec-key-file'))} "
        )
        controller_cmd = (
            common + env +
            f"{shlex.quote(str(REPO / 'build/examples/App_ServiceController'))} "
            "--controller-prefix /example/python-ndn/controller "
            f"--policy-file {shlex.quote(str(REPO / 'examples/python-ndn-stream.policies'))} "
            f"--trust-schema {shlex.quote(str(REPO / 'examples/trust-schema.conf'))} "
            f">{shlex.quote(str(output / 'controller.stdout'))} "
            f"2>{shlex.quote(str(output / 'controller.stderr'))}"
        )
        controller_process = provider.popen(controller_cmd, shell=True)
        time.sleep(1.0)
        if controller_process.poll() is not None:
            raise RuntimeError("controller exited before Python Provider startup")

        test_cmd = (
            common + env +
            "NDNSF_RUN_PYTHON_NDN_STREAM_INTEGRATION=1 "
            "python3 tests/python/run_python_ndn_stream_push.py "
            f">{shlex.quote(str(output / 'test.stdout'))} "
            f"2>{shlex.quote(str(output / 'test.stderr'))}"
        )
        (output / "commands.json").write_text(
            json.dumps({"controller": controller_cmd, "test": test_cmd}, indent=2)
            + "\n", encoding="utf-8")
        test_process = provider.popen(test_cmd, shell=True)
        test_process.wait(timeout=30)
        test_rc = test_process.returncode
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        test_rc = test_rc if test_process is None else stop_process(test_process)
        controller_rc = stop_process(controller_process)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                error = error or f"ndn.stop: {type(exc).__name__}: {exc}"
        try:
            Minindn.cleanUp()
        finally:
            sys.argv = original_argv

    summary = {
        "schemaVersion": "spec173-python-ndn-stream-push-v1",
        "passed": not error and test_rc == 0,
        "testReturnCode": test_rc,
        "controllerReturnCode": controller_rc,
        "runtimeSeconds": round(time.monotonic() - started, 3),
        "pib": str(pib),
        "tpm": str(tpm),
        "error": error,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output", type=Path,
        default=Path("/tmp/ndnsf-python-ndn-stream-push"),
    )
    args = parser.parse_args()
    summary = run(args.output.resolve())
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
