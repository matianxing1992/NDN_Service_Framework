#!/usr/bin/env python3
"""Prepare then qualify an authorized or withdrawn stored-model run on installed MiniNDN."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

import admission
import withdrawal


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def documents(root):
    prepare, stored, reference = (
        json.loads((root / name).read_text())
        for name in ("prepare.json", "stored.json", "oracle-source.json")
    )
    positive = stored["requester"]
    original = prepare["requester"]
    require(prepare["platform"] == stored["platform"] and
            stored["platform"]["kind"] == "minindn", "the phases need the same MiniNDN topology")
    require(prepare["storage"] == stored["storage"] and prepare["release"] == stored["release"],
            "the phases need the same installed candidate and Repo volume")
    require(original["model_input"]["mode"] == "publish-stored" and
            positive["model_input"]["mode"] == "stored", "expected native publish and stored modes")
    for field in ("private_handle_file", "repo_target"):
        require(original["model_input"][field] == positive["model_input"][field],
                "stored input does not identify the original publication destination")
    require(not {"file", "initializer_file"}.intersection(positive["catalog"]["source"]),
            "stored requester has source inputs")
    journal = Path(positive["conversation"]["journal"]["state_root"])
    require(journal.is_absolute() and not any(base == journal.resolve() or
            base in journal.resolve().parents for base in map(Path, ("/tmp", "/run", "/dev/shm"))),
            "continuity needs a durable conversation journal")
    require(not stored["security"].get("controller_recovery_bootstrap") and
            prepare["security"]["controller_recovery_checkpoint"] ==
            stored["security"]["controller_recovery_checkpoint"] and
            prepare["security"]["controller_policy"] == stored["security"]["controller_policy"],
            "invalid Controller recovery inputs")
    assignments = positive["fixed_placement"]["role_providers"]
    providers = {actor["identity"]: actor["node"] for actor in stored["actors"]
                 if actor["kind"] == "provider" and not actor.get("external")}
    require(positive["catalog"]["splitter"]["tensor_degrees"] == [2, 1] and
            len(positive["turns"]) == 2 and len(assignments) == 3 and
            len({providers[name] for name in assignments.values()}) == 3,
            "expected two turns on three independent Provider ranks")
    source = Path(original["catalog"]["source"]["file"]).resolve(strict=True)
    reference_file = Path(reference["source"]["file"]).resolve(strict=True)
    require(root in source.parents and source != reference_file and
            digest(source) == digest(reference_file) == reference["source"]["digest"].split(":")[1],
            "private source copy or independent ONNX identity differs")
    controller = next(actor for actor in stored["actors"] if actor["kind"] == "controller")
    requester = next(actor for actor in stored["actors"] if actor["kind"] == "requester")
    target = controller["parameters"]
    require(target["revoke-kind"] == "service" and
            target["revoke-identity"] == requester["identity"] and
            target["revoke-service"] != stored["network"]["service"] and
            target["revoke-attribute"] == "/PERMISSION" + target["revoke-service"],
            "the revocation must target the separate service")
    # Inspect the tracked fixture's generated allow lists before attributing a
    # Controller epoch increase to withdrawal of an existing permission.
    policy = Path(stored["security"]["controller_policy"]).read_text()
    entries = re.findall(r"(?m)^\s*(user|provider)-policy\s*\{\s*for\s+(\S+)\s+allow\s*\{([^{}]*)\}", policy)
    holders = {(kind, identity) for kind, identity, scopes in entries
               if target["revoke-service"] in scopes.split()}
    require(("user", requester["identity"]) in holders and any(kind == "provider" for kind, _ in holders),
            "separate service lacks an initial User/Provider grant in the generated policy")
    return prepare, stored, source, target


def invoke(command, root, name, expected_exit=0):
    output_path = root / (name + ".stdout")
    error_path = root / (name + ".stderr")
    with output_path.open("x") as output, error_path.open("x") as errors:
        completed = subprocess.run(command, cwd=root, stdout=output, stderr=errors)
    require(completed.returncode == expected_exit,
            f"{name} failed with exit {completed.returncode}; see {root}")
    return (root / (name + ".stdout")).read_text()


def phase(root, config, config_name, action, execution):
    launcher = Path(config["release"]["prefix"]) / "bin/ndnsf-di"
    result = json.loads(invoke([str(launcher), "cluster", action, "--config",
                                str(root / config_name), "--debug", "--timing"],
                               root, action))
    require(result["operation_stage"] == "STOPPED" and result["execution"] == execution and
            not result["failure"] and result["cleanup"] and
            all(actor["status"] == "ZERO" and actor["forced"] is False
                for actor in result["cleanup"]), f"{action} did not finish and clean up normally")
    actor_ids = {actor["id"] for actor in config["actors"] if not actor.get("external", False)}
    reported = {row["id"]: row for row in result["cleanup"] if row["id"] in actor_ids}
    require(reported and all(type(row.get("exitCode")) is int and row["exitCode"] == 0
                             for row in reported.values()) and
            (action != "up" or reported.keys() == actor_ids),
            f"{action} is missing successful actor exits")
    evidence = Path(result["evidence_root"]).resolve(strict=True)
    require(root in evidence.parents, f"{action} evidence escaped its fixture")
    return evidence


def snapshot_objects(repo):
    store = repo / "repo.sqlite.store" / "payloads" / "sha256"
    files = {str(file.relative_to(store)): digest(file)
             for file in store.rglob("*") if file.is_file() and not file.is_symlink()}
    require(files, "the persistent Repo has no committed payload files")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--check-fixture", action="store_true")
    parser.add_argument("--case", choices=("continuity", "withdrawal", "user-withdrawal", "invalid-grant",
                                           "partition-active", "missing-rank"),
                        default="continuity")
    parser.add_argument("--withdrawal-period-ms", type=int, choices=(1000, 2000), default=2000)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    require(root.stat().st_uid == os.geteuid() and stat.S_IMODE(root.stat().st_mode) == 0o700,
            "fixture must be private and owned by the launcher")
    prepare, stored, source, target = documents(root)
    if args.check_fixture:
        print("CONTINUITY_FIXTURE_OK", root)
        return

    handle = Path(stored["requester"]["model_input"]["private_handle_file"])
    repo = Path(stored["storage"]["repo_root"])
    checkpoint = Path(stored["security"]["controller_recovery_checkpoint"])
    trigger = Path(target["revoke-trigger-file"])
    offline_source = source.with_name(source.name + ".offline")
    fresh = [handle, repo, checkpoint, trigger, offline_source]
    fresh.extend(root / (name + suffix) for name in ("prepare", "up", "oracle")
                 for suffix in (".stdout", ".stderr"))
    require(all(path.is_absolute() and root in path.resolve().parents and
                not path.exists() and not path.is_symlink() for path in fresh),
            "continuity drivers require fresh destinations")
    os.umask(0o077)
    manifest = Path(stored["release"]["manifest"])
    candidate = digest(manifest)
    old_evidence = phase(root, prepare, "prepare.json", "prepare", "PREPARED")
    receipts = [json.loads(line) for line in (old_evidence / "requester.log").read_text().splitlines()
                if line.startswith("{") and '"ndnsf-di-stored-model-preparation"' in line]
    require(len(receipts) == 1 and receipts[0]["status"] == "PUBLISHED" and
            receipts[0]["model"]["rootWireDigest"], "missing native publication receipt")
    with handle.open("rb") as stream:
        require(stream.readline() == b"NDNSF-DI-STORED-MODEL-HANDLE-v2\n",
                "the prepare phase did not create a sealed model handle")
    handle_digest = digest(handle)
    old_objects = snapshot_objects(repo)
    require(checkpoint.is_file() and digest(manifest) == candidate,
            "the durable Controller checkpoint or installed release changed")
    source.rename(offline_source)
    if args.case != "continuity":
        launcher = Path(stored["release"]["prefix"]) / "bin/ndnsf-di"
        if args.case == "missing-rank":
            roles = stored["requester"]["fixed_placement"]["role_providers"]
            paired, = (a for a in stored["actors"] if a["id"] == "provider-1" and a["kind"] == "provider")
            role, = (r for r, identity in roles.items() if identity == paired["identity"])
            result = withdrawal.run(root, stored, role, launcher, missing_rank=True)
        elif args.case in {"withdrawal", "partition-active"}:
            stored["requester"]["request"]["policy_revalidation_period_ms"] = args.withdrawal_period_ms
            target_role = next(iter(stored["requester"]["fixed_placement"]["role_providers"]))
            if args.case == "partition-active":
                result = withdrawal.run(root, stored, target_role, launcher, partition=True)
            else:
                result = withdrawal.run(root, stored, target_role, launcher)
            result["revalidationPeriodMs"] = stored["requester"]["request"]["policy_revalidation_period_ms"]
        else:
            grant_target = admission.configure(root, stored, args.case)
            config_path = root / (args.case + "-deployment.json")
            with config_path.open("x") as output:
                json.dump(stored, output)
            trigger.touch(exist_ok=False)
            cmd = [str(launcher), "cluster", "up", "--config", str(config_path),
                   "--debug", "--timing", "--timeout", "900"]
            launch = json.loads(invoke(cmd, root, "up", expected_exit=1))
            evidence = Path(launch["evidence_root"]).resolve(strict=True)
            require(root in evidence.parents and launch["failure"] == "REQUESTER_EXIT:1",
                    "denial did not come from this fixture's Requester")
            actual = json.loads((evidence / "configuration.json").read_text())
            require(actual["release"] == stored["release"] and
                    actual["requester"]["model_input"] == stored["requester"]["model_input"] and
                    actual["requester"]["fixed_placement"] == stored["requester"]["fixed_placement"] and
                    actual["artifact_authority"]["authority"]["recipient_public_key_files"] ==
                    stored["artifact_authority"]["authority"]["recipient_public_key_files"],
                    "candidate, model, assignments or Grant recipient inputs changed at launch")
            result = admission.verify(actual, evidence, args.case, grant_target, trigger.stat().st_mtime_ns)
            if grant_target:
                result["originalRecipientPublicKeySha256"] = digest(Path(grant_target["originalPublicKey"]))
                result["submittedRecipientPublicKeySha256"] = digest(Path(grant_target["submittedPublicKey"]))
            result["configurationSha256"] = digest(evidence / "configuration.json")
        require(candidate == digest(manifest) and handle_digest == digest(handle),
                "installed release or original sealed handle changed")
        new_objects = snapshot_objects(repo)
        if args.case in {"withdrawal", "partition-active", "missing-rank"}:
            require(all(new_objects.get(name) == wire for name, wire in old_objects.items()),
                    "original Repo packets changed during withdrawal")
        else:
            require(new_objects == old_objects, "a denied run changed the Repo payload inventory")
        result.update(phase=args.case,
                      releaseSha256=candidate, handleSha256=handle_digest,
                      rootWireDigest=receipts[0]["model"]["rootWireDigest"], prepareEvidence=str(old_evidence))
        result_path = root / (args.case + "-result.json")
        with result_path.open("x") as output:
            json.dump(result, output, indent=2, sort_keys=True)
            output.write("\n")
        print("THREE_RANK_ROLLOVER_PROBE=PASS", result_path)
        return
    trigger.touch(exist_ok=False)
    new_evidence = phase(root, stored, "stored.json", "up", "COMPLETE")
    require(digest(manifest) == candidate and digest(handle) == handle_digest,
            "same-release and same-handle continuity did not hold")
    new_objects = snapshot_objects(repo)
    require(all(new_objects.get(name) == wire for name, wire in old_objects.items()),
            "a prepared Repo payload changed or disappeared")
    controller_log = (new_evidence / "controller.log").read_text()
    generation, epoch, _ = withdrawal.committed_revocation(
        controller_log.splitlines(), target["revoke-identity"], target["revoke-service"], trigger.stat().st_mtime_ns)
    service = stored["network"]["service"]
    require(re.search(r"NDNSF_POLICY_STATUS_PUBLISHED service=" + re.escape(service) +
                      rf" generation={generation} epoch={epoch}(?:\s|$)", controller_log),
            "inference service has no signed status at the withdrawal version")
    oracle = Path(stored["release"]["prefix"]) / "bin/multiturn-oracle"
    expected_oracle = json.loads(manifest.read_text())["files"][str(oracle)]["installed"]
    require(expected_oracle["kind"] == "file" and not oracle.is_symlink() and
            expected_oracle["sha256"] == digest(oracle), "the C++ oracle is outside the release")
    prefix = "SPEC220_CPP_MODEL_REFERENCE_PASS "
    output = invoke([str(oracle), "--deployment-state", str(new_evidence / "operation.json"),
                     "--model-reference", "--model-reference-source", str(root / "oracle-source.json"),
                     "--require-exact-conversation", "32", "3"], root, "oracle")
    lines = [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]
    require(len(lines) == 1, "missing independent ONNX oracle receipt")
    result = json.loads(lines[0])
    require(result["exactConversation"]["tokensPerTurn"] == 32 and
            result["exactConversation"]["providersPerTurn"] == 3 and
            len(result["modelReference"]["rounds"]) == 2 and
            result["modelReference"]["providerStateValuesCompared"] and
            result["modelReference"]["providerLogitsValueCount"] > 0,
            "independent oracle did not evaluate both token, logits and state sequences")
    with (root / "continuity-result.json").open("x") as stream:
        json.dump({"phase": "continuity", "releaseSha256": candidate, "handleSha256": handle_digest,
                   "rootWireDigest": receipts[0]["model"]["rootWireDigest"],
                   "prepareEvidence": str(old_evidence), "evidence": str(new_evidence),
                   "oracle": result}, stream, sort_keys=True, indent=2)
        stream.write("\n")
    print("STORED_MODEL_CONTINUITY_PASS", receipts[0]["model"]["rootWireDigest"], new_evidence)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as failure:
        raise SystemExit(str(failure))
