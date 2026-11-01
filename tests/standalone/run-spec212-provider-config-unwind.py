#!/usr/bin/env python3
"""Check actual Provider configuration-return lifetime on a private local NFD.

Python owns processes and receipts; the production C++ executable validates its
Owner configuration. This is neither a Face-drain nor a MiniNDN inference test.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            result.update(block)
    return result.hexdigest()


def process_helpers():
    path = ROOT / "tests/standalone/run-spec182-native-grant-process.py"
    spec = importlib.util.spec_from_file_location("spec182_process_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def project_libraries(executable, env, expected_root, sanitized):
    result = subprocess.run(["/usr/bin/ldd", str(executable)], env=env, check=True,
                            capture_output=True, text=True, timeout=15)
    if "not found" in result.stdout:
        raise RuntimeError("Provider library closure is incomplete")
    libraries = {}
    for name, location in re.findall(r"^\s*(\S+) => (\S+) ", result.stdout, re.M):
        if name.startswith(("libndnsf-", "libndn-service-framework")):
            path = Path(location).resolve(strict=True)
            if expected_root and expected_root not in path.parents:
                raise RuntimeError("Project library resolved outside the requested build tree")
            libraries[name] = {"path": str(path), "sha256": digest(path)}
    if not any(n.startswith("libndnsf-distributed-inference") for n in libraries) or not any(
            n.startswith("libndn-service-framework") for n in libraries):
        raise RuntimeError("Expected DI/Core dynamic library closure is absent")
    if sanitized:
        for path in [executable] + [Path(value["path"]) for value in libraries.values()]:
            symbols = subprocess.check_output(["/usr/bin/nm", "-D", str(path)], text=True,
                                              timeout=30)
            if "__asan_init" not in symbols or "__ubsan_handle_" not in symbols:
                raise RuntimeError("ASAN/UBSAN instrumentation is absent: " + str(path))
    return libraries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provider-binary", type=Path,
                        default=Path("/usr/local/bin/di-native-provider"))
    parser.add_argument("--profile", choices=("normal", "asan-ubsan"), required=True)
    parser.add_argument("--library-dir", type=Path)
    for option in ("service", "provider", "group", "controller", "roles"):
        parser.add_argument("--" + option, required=True)
    args = parser.parse_args()
    campaign = args.campaign.resolve(strict=True)
    work = args.work_root.resolve(strict=True)
    output = args.output.resolve()
    executable = args.provider_binary.resolve(strict=True)
    if os.environ.get("NDNSF_DI_WORKSPACE") != str(work):
        raise RuntimeError("Run inside the matching locked ndnsf-di-workspace helper")
    if work == campaign or campaign in work.parents or work in campaign.parents:
        raise RuntimeError("Campaign inputs and disposable work must be separate")
    if output == work or work in output.parents or output == campaign or campaign in output.parents:
        raise RuntimeError("Evidence output must be separate from work and input campaign")
    if work.stat().st_uid != os.geteuid() or stat.S_IMODE(work.stat().st_mode) & 0o077:
        raise RuntimeError("Work root must be owned and private")
    if not os.access(executable, os.X_OK):
        raise RuntimeError("Provider is not executable")
    sanitized = args.profile == "asan-ubsan"
    library_dir = args.library_dir.resolve(strict=True) if args.library_dir else None
    if sanitized and (not library_dir or library_dir not in executable.parents):
        raise RuntimeError("ASAN mode requires a build-tree executable and library directory")
    helper = process_helpers()
    socket = helper.bounded_nfd_socket(work)
    if socket.parent != work:
        raise RuntimeError("Use a short helper purpose so the NFD socket stays private")
    nfd_binary = shutil.which("nfd", path="/usr/local/bin:/usr/bin:/bin")
    if not nfd_binary:
        raise RuntimeError("NFD is unavailable")
    inputs = {
        "plan": campaign / "native-qwen-execution-plan.json",
        "manifest": campaign / "native-qwen-service-manifest.json",
        "trust-schema": campaign / "trust-schema.conf",
        "kv-handoff-config": campaign / "provider-0/handoff.json",
    }
    recovery_path = campaign / "provider-0/recovery.json"
    recovery = json.loads(recovery_path.read_text())
    handoff = json.loads(inputs["kv-handoff-config"].read_text())
    certificates = [Path(p) for p in handoff["owner_certificates"]]
    certificates.append(Path(handoff["owner_issuer_certificate"]))
    controller_certificate = Path(json.loads((campaign / "result.json").read_text())[
        "identities"]["controller_certificate_file"])
    certificates.append(controller_certificate)
    if any(not p.is_absolute() for p in certificates):
        raise RuntimeError("This driver requires the campaign's absolute public certificate paths")
    readonly = list(inputs.values()) + [recovery_path] + certificates
    hashes = {str(p): digest(p) for p in readonly}
    output.mkdir(mode=0o700)  # Refuse to overwrite an earlier receipt.
    receipt = {"status": "RUNNING", "profile": args.profile, "input_hashes": hashes,
               "executable": str(executable), "executable_sha256": digest(executable),
               "scope": "LOCAL_CONFIG_UNWIND_ONLY", "iterations": [], "first_failure": None}
    provider = nfd = None
    children = []
    env = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "ASAN_OPTIONS", "LSAN_OPTIONS",
                "UBSAN_OPTIONS", "TSAN_OPTIONS"):
        env.pop(key, None)
    env["HOME"] = str(work / "nfd-home")
    Path(env["HOME"]).mkdir(mode=0o700)
    env["NDN_CLIENT_PIB"] = "pib-sqlite3:" + str(work / "nfd-pib")
    env["NDN_CLIENT_TPM"] = "tpm-file:" + str(work / "nfd-tpm")
    env["NDNSF_CONFIG"] = str(work / "nfd-ndnsf.conf")
    env["NDN_CLIENT_TRANSPORT"] = "unix://" + str(socket)
    helper.write(output / "result.json", json.dumps(receipt, indent=2) + "\n", 0o600)

    def interrupted(signum, _frame):
        raise InterruptedError("process driver received signal " + str(signum))

    previous_signal = signal.signal(signal.SIGTERM, interrupted)
    try:
        helper.nfd_config(work / "nfd.conf", socket)
        nfd = helper.run([nfd_binary, "--config", str(work / "nfd.conf")], env,
                         output / "nfd.log", wait=False)
        children.append(nfd)
        deadline = time.monotonic() + 10
        while not socket.exists():
            if nfd.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Private NFD did not open its socket within10s")
            time.sleep(0.05)
        if nfd.poll() is not None:
            raise RuntimeError("Private NFD exited after creating its socket")
        for index in range(3):
            if nfd.poll() is not None:
                raise RuntimeError("Private NFD exited before Provider launch")
            private = work / ("provider-" + str(index))
            private.mkdir(mode=0o700)
            (private / "home").mkdir(mode=0o700)
            (private / "kv").mkdir(mode=0o700)
            copied_recovery = dict(recovery, privateScratchRoot=str(private / "kv"))
            helper.write(private / "recovery.json", json.dumps(copied_recovery), 0o600)
            copied_handoff = dict(handoff)
            if index == 0:
                copied_handoff.pop("repo_ack_collection_ms", None)
            else:
                copied_handoff["repo_ack_collection_ms"] = 3000
            expected_ack_ms = 1000 if index == 0 else 3000
            helper.write(private / "handoff.json", json.dumps(copied_handoff), 0o600)
            child_env = dict(env, HOME=str(private / "home"),
                             NDNSF_CONFIG=str(private / "ndnsf.conf"),
                             NDN_LOG="ndnsf.di.RuntimeEvidence=WARN",
                             NDN_CLIENT_PIB="pib-sqlite3:" + str(private / "pib"),
                             NDN_CLIENT_TPM="tpm-file:" + str(private / "tpm"),
                             NDNSF_CONTROLLER_GENERATION_STATE=str(private / "controller-generation.state"),
                             NDNSF_CONTROLLER_CERT_FILE=str(controller_certificate))
            if library_dir:
                child_env["LD_LIBRARY_PATH"] = str(library_dir)
            if sanitized:
                child_env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                                 UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            libraries = project_libraries(executable, child_env, library_dir, sanitized)
            if libraries != receipt.get("project_libraries", libraries):
                raise RuntimeError("Project library identity changed between children")
            receipt["project_libraries"] = libraries
            command = [str(executable), "--serve", "--check-kv-handoff-config"]
            for option, path in inputs.items():
                if option == "kv-handoff-config":
                    path = private / "handoff.json"
                command += ["--" + option, str(path)]
            for option in ("service", "provider", "group", "controller", "roles"):
                command += ["--" + option, getattr(args, option)]
            command += ["--kv-recovery-config", str(private / "recovery.json"),
                        "--artifact-cache-dir", str(private / "cache"),
                        "--offer-backend", "onnxruntime-cpu"]
            log = output / ("provider-" + str(index) + ".log")
            provider = helper.run(command, child_env, log, wait=False)
            children.append(provider)
            code = provider.wait(timeout=120)
            provider._ndnsf_log_handle.close()
            record = {"index": index, "argv": command, "exit": code,
                      "libraries": libraries, "recovery_sha256": digest(private / "recovery.json"),
                      "handoff_sha256": digest(private / "handoff.json"),
                      "expected_repo_ack_ms": expected_ack_ms}
            receipt["iterations"].append(record)
            text = log.read_text(errors="replace")
            if code or "NDNSF_DI_HANDOFF_CONFIG_CHECK_PASS" not in text or \
                    "NDNSF_DI_HANDOFF_CONFIG_VALIDATED" not in text:
                raise RuntimeError("Actual Provider configuration-return check failed")
            if not re.search(r"NDNSF_DI_HANDOFF_CONFIG_VALIDATED[^\n]* repoAckMs=" +
                             str(expected_ack_ms) + r"(?:\s|$)", text):
                raise RuntimeError("Actual Provider Repo ACK budget did not match configuration")
            if re.search(r"AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:", text):
                raise RuntimeError("Sanitizer diagnostic in Provider log")
            if any(marker in text for marker in (
                    "NDNSF_DI_NETWORK_PROBE_REGISTERED", "NDNSF_DI_ASSEMBLY_PROGRESS",
                    "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_FETCH_ISSUED")):
                raise RuntimeError("Provider passed the intended early-return boundary")
        if nfd.poll() is not None:
            raise RuntimeError("Private NFD exited before process checks completed")
        receipt["status"] = "CONFIG_UNWIND_PROCESS_PASS_NOT_MININDN_ACCEPTANCE"
    except (Exception, KeyboardInterrupt) as error:
        receipt["status"] = "FAIL_STOPPED"
        receipt["first_failure"] = type(error).__name__ + ": " + str(error)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        cleanup_errors = []
        for child in reversed(children):
            try:
                helper.stop(child)
                child._ndnsf_log_handle.close()
                if child.poll() is None:
                    cleanup_errors.append("owned child remains")
            except Exception as error:
                cleanup_errors.append(type(error).__name__ + ": " + str(error))
        if socket.exists():
            info = socket.lstat()
            if stat.S_ISSOCK(info.st_mode) and info.st_uid == os.geteuid():
                socket.unlink()
            else:
                cleanup_errors.append("unexpected socket replacement")
        receipt["cleanup_errors"] = cleanup_errors
        receipt["owned_process_exits"] = [
            {"pid": child.pid, "exit": child.poll(),
             "log": Path(child._ndnsf_log_handle.name).name} for child in children]
        try:
            receipt["inputs_unchanged"] = all(digest(Path(p)) == value for p, value in hashes.items())
            receipt["artifacts_unchanged"] = digest(executable) == receipt["executable_sha256"] and all(
                digest(Path(value["path"])) == value["sha256"]
                for value in receipt.get("project_libraries", {}).values())
        except OSError as error:
            receipt["inputs_unchanged"] = False
            receipt["artifacts_unchanged"] = False
            cleanup_errors.append("input recheck: " + str(error))
        receipt["cleanup"] = "FAIL" if cleanup_errors else "PASS"
        if cleanup_errors or not receipt["inputs_unchanged"] or not receipt["artifacts_unchanged"]:
            receipt["status"] = "FAIL_STOPPED"
            receipt["first_failure"] = receipt["first_failure"] or "cleanup/input identity failed"
        helper.write(output / "result.json", json.dumps(receipt, indent=2) + "\n", 0o600)
        signal.signal(signal.SIGTERM, previous_signal)
    print(json.dumps({"status": receipt["status"], "receipt": str(output / "result.json")}))
    return 0 if receipt["status"].startswith("CONFIG_UNWIND_PROCESS_PASS") else 1


if __name__ == "__main__":
    raise SystemExit(main())
