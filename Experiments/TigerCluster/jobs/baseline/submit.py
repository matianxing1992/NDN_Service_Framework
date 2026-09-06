#!/usr/bin/env python3
"""One launcher for baseline and subsequent service-echo experiments.

prepare prints the exact submission without side effects; submit uses it once.
local runs two logical nodes in the same SIF and can produce only LOCAL_PASS.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys

BUNDLE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BUNDLE))
from runtime.baseline import (PYTHON, bundle_files, collect, container_command,
                              container_env, digest, load_profile, write_json)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def public_config(public, namespace):
    public.mkdir()
    # Only signed keys below this run's root can validate. The application
    # namespace deliberately excludes all other jobs and /example defaults.
    components = "".join("<" + x + ">" for x in namespace.strip("/").split("/"))
    (public / "trust.conf").write_text(f'''
rule
{{
 id certificates
 for data
 filter
 {{
  type name
  regex ^{components}<>*<KEY><><><>$
 }}
 checker
 {{
  type hierarchical
  sig-type rsa-sha256
 }}
}}
rule
{{
 id runtime
 for data
 filter
 {{
  type name
  regex ^{components}<>*$
 }}
 checker
 {{
  type customized
  sig-type rsa-sha256
  key-locator
  {{
   type name
   regex ^{components}<>*<KEY><>{{1,3}}$
  }}
 }}
}}
rule
{{
 id sync
 for interest
 filter
 {{
  type name
  regex ^{components}<group><>*$
 }}
 checker
 {{
  type customized
  sig-type rsa-sha256
  key-locator
  {{
   type name
   regex ^{components}<>*<KEY><>{{1,3}}$
  }}
 }}
}}
trust-anchor
{{
 type file
 file-name /config/root.cert
}}
''')
    (public / "trust-wrong.conf").write_text((public / "trust.conf").read_text().replace(
        "file-name /config/root.cert", "file-name /config/wrong-root.cert"))
    (public / "bootstrap-tokens.txt").write_text("# Offline certificate provisioning; no online enrollment tokens.\n")
    (public / "policy.conf").write_text(f'''
name {namespace}/controller/NDNSF/ControllerPolicy/v1
provider-policies
{{
 provider-policy
 {{
  for {namespace}/provider
  allow
  {{
   /TIGER_ECHO
   /TIGER_CONTROL
  }}
 }}
}}
user-policies
{{
 user-policy
 {{
  for {namespace}/user
  allow
  {{
   /TIGER_ECHO
  }}
 }}
 user-policy
 {{
  for {namespace}/denied
  allow
  {{
   /TIGER_CONTROL
  }}
 }}
}}
''')


def validate_options(args):
    if not re.fullmatch(r"[a-z][a-z0-9-]{1,47}", args.run_id):
        raise ValueError("RUN_ID")
    for path in (args.output.resolve(), args.profile.resolve(), BUNDLE):
        if any(c in str(path) for c in ":,\n\r"):
            raise ValueError("UNSAFE_PATH")
    profile = load_profile(args.profile)
    if args.action == "local":
        if not args.sif or not args.apptainer:
            raise ValueError("LOCAL_REQUIRES_EXPLICIT_SIF_AND_APPTAINER")
        profile["sif"] = str(args.sif.resolve())
        profile["apptainer"] = str(args.apptainer.resolve())
        version = subprocess.check_output([profile["apptainer"], "--version"], text=True).strip()
        if not version.startswith("apptainer version "):
            raise ValueError("APPTAINER_VERSION_OUTPUT")
        profile["apptainerVersion"] = version[len("apptainer version "):]
    elif args.sif or args.apptainer:
        raise ValueError("SLURM_RUNTIME_COMES_FROM_PROFILE")
    return profile


def submission(args, profile):
    """Return the exact argv; no shell interpolation or ambient resource flags."""
    return ["sbatch", "--parsable", "--export=NONE", "--partition=" + profile["partition"],
            "--account=" + profile["account"], "--nodes=2", "--ntasks=2", "--ntasks-per-node=1",
            "--cpus-per-task=" + str(profile["cpusPerNode"]), "--mem=" + profile["memory"],
            "--time=" + profile["wallTime"], "--job-name=tiger-" + args.workload,
            "--output=" + str(args.output.resolve() / (args.run_id + "-slurm-%j.log")),
            str(BUNDLE / "jobs/baseline/run.sbatch"), str(BUNDLE), str(args.profile.resolve()),
            str(args.output.resolve()), args.run_id, args.workload]


def run(args, profile):
    if args.action == "run" and not os.environ.get("SLURM_JOB_ID"):
        raise ValueError("ALLOCATION_REQUIRED")
    if digest(Path(profile["sif"])) != profile["sifSha256"]:
        raise ValueError("SIF_DIGEST_MISMATCH")
    if args.action == "run":
        receipt = json.loads((args.output / (args.run_id + "-submission.json")).read_text())
        if (receipt["profileSha256"] != digest(args.profile) or
                receipt["bundleFiles"] != bundle_files(BUNDLE)):
            raise ValueError("SUBMITTED_INPUTS_CHANGED")
    root = args.output.resolve() / args.run_id
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    private, public = root / "private", root / "public"
    private.mkdir(mode=0o700)
    (private / "issuer").mkdir(mode=0o700)
    record = {"schema": "tiger-baseline-run-v1", "runId": args.run_id,
              "mode": "local" if args.action == "local" else "slurm",
              "workload": args.workload, "namespace": "/example/tiger/" + args.run_id,
              "payload": "hello-tiger" if args.workload == "baseline" else "reused-runtime",
              "startedAt": utc(), "profile": profile,
              "profileSha256": digest(args.profile), "bundle": str(BUNDLE),
              "bundleFiles": bundle_files(BUNDLE), "sifSha256": profile["sifSha256"],
              "slurmJobId": os.environ.get("SLURM_JOB_ID"), "status": "RUNNING"}
    git = subprocess.run(["git", "-C", str(BUNDLE), "rev-parse", "HEAD"],
                         capture_output=True, text=True)
    record["sourceCommit"] = git.stdout.strip() if git.returncode == 0 else None
    processes = []
    nodes = []
    rc = 1
    def cancelled(*_):
        raise InterruptedError("COORDINATOR_CANCELLED")
    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    try:
        public_config(public, record["namespace"])
        if args.action == "local":
            record["nodes"] = [{"hostname": socket.gethostname(), "address": "127.0.0.1",
                                 "port": profile["tcpPort"] + rank} for rank in range(2)]
        else:
            hosts = subprocess.check_output(["scontrol", "show", "hostnames", os.environ["SLURM_JOB_NODELIST"]],
                                            text=True).splitlines()
            if len(hosts) != 2 or len(set(hosts)) != 2:
                raise ValueError("EXACTLY_TWO_NODES_REQUIRED")
            record["nodes"] = [{"hostname": host, "address": socket.gethostbyname(host),
                                 "port": profile["tcpPort"]} for host in hosts]
        write_json(root / "request.json", record)
        with (root / "identity.log").open("wb") as log:
            setup = subprocess.run(container_command(profile, BUNDLE, private / "issuer", public,
                      root, [PYTHON, "/bundle/runtime/identities.py", record["namespace"]], prepare=private),
                      stdout=log, stderr=subprocess.STDOUT, timeout=120, env=container_env())
        if setup.returncode:
            raise RuntimeError("IDENTITY_SETUP_FAILED")
        record["publicFiles"] = {p.name: digest(p) for p in public.iterdir() if p.is_file()}
        write_json(root / "request.json", record)
        env = dict(os.environ, PYTHONPATH=str(BUNDLE), PYTHONNOUSERSITE="1")
        if args.action == "local":
            for rank in range(2):
                log = (root / f"worker-{rank}.log").open("wb")
                proc = subprocess.Popen([sys.executable, "-m", "runtime.worker",
                                         str(root / "request.json"), "--rank", str(rank)],
                                        env=env, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
                processes.append((proc, log))
        else:
            log = (root / "workers.log").open("wb")
            proc = subprocess.Popen(["srun", "--kill-on-bad-exit=1", "--nodes=2", "--ntasks=2",
                                     "--ntasks-per-node=1", "/usr/bin/python3", "-m", "runtime.worker",
                                     str(root / "request.json")], env=env, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
            processes.append((proc, log))
        codes = [proc.wait(timeout=420) for proc, _ in processes]
        rc = 0 if all(code == 0 for code in codes) else 1
    except BaseException as exc:
        record["error"] = type(exc).__name__ + ":" + str(exc)
    finally:
        for proc, log in processes:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=5)
            log.close()
        for rank in range(2):
            path = root / f"node{rank}/result.json"
            if path.exists():
                nodes.append(json.loads(path.read_text()))
        shutil.rmtree(private)
        record["privateRemoved"] = not private.exists()
        record["finishedAt"] = utc()
        record = collect(record, nodes, rc)
        write_json(root / "run.json", record)
        print(json.dumps({"record": str(root / "run.json"), "status": record["status"],
                          "error": record.get("error"), "problems": record["problems"]}))
    return 0 if record["status"] in ("PASS", "LOCAL_PASS") else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["prepare", "submit", "run", "local"])
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--workload", choices=["baseline", "service-echo"], default="baseline")
    parser.add_argument("--sif", type=Path)
    parser.add_argument("--apptainer", type=Path)
    args = parser.parse_args()
    profile = validate_options(args)
    if args.action in ("prepare", "submit"):
        cmd = submission(args, profile)
        plan = {"schema": "tiger-baseline-submission-v1", "argv": cmd,
                "profileSha256": digest(args.profile), "bundleFiles": bundle_files(BUNDLE)}
        if args.action == "submit":
            args.output.mkdir(parents=True, exist_ok=True)
            receipt = args.output / (args.run_id + "-submission.json")
            # An exclusive receipt prevents an uncertain SSH result from causing
            # an automatic duplicate sbatch. Reconcile the first job instead.
            with receipt.open("x") as stream:
                json.dump({**plan, "status": "SUBMITTING"}, stream)
            completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
            plan.update(exitCode=completed.returncode, stdout=completed.stdout.strip(),
                        stderr=completed.stderr.strip(), status="SUBMITTED" if completed.returncode == 0 else "FAILED")
            write_json(receipt, plan)
            print(json.dumps(plan))
            return completed.returncode
        print(json.dumps(plan, indent=2))
        return 0
    return run(args, profile)


if __name__ == "__main__":
    raise SystemExit(main())
