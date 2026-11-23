"""Observe and qualify withdrawal during the second turn of an installed DI deployment."""

import json
import os
import re
import subprocess
import time
from decimal import Decimal
from pathlib import Path


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def records(lines, marker):
    for line in lines:
        head, found, tail = line.partition(marker + " ")
        if found:
            stamp = re.match(r"^(\d{10}\.\d+)\s", head)
            ns = int(Decimal(stamp[1]) * 1_000_000_000) if stamp else None
            yield ns, dict(word.split("=", 1) for word in tail.split() if "=" in word)


def lines(path):
    if not path.is_file():
        return []
    with path.open(errors="replace") as stream:
        return [line for line in stream if useful(line)]


def useful(line):
    return (any(marker in line for marker in ("NATIVE_CONVERSATION_TURN_",
                "NDNSF_DI_NATIVE_SELECTION_ACCEPTED ", "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED ",
                "NDNSF_DI_REQUEST_TERMINAL ", "NDNSF_CONTROLLER_REVOKED ", "NDNSF_REVOCATION_APPLIED ",
                "NDNSF_PROVIDER_AUTHORITY_WITHDRAWN ", "NDNSF_DI_NATIVE_PROVIDER_AUTHORITY_WITHDRAWN ",
                "Installed PolicyStatus ", "NDNSF_CONTROLLER_CACHE_INVALIDATED ",
                "NDNSF_DI_NATIVE_PROVIDER_SHUTDOWN_REQUESTED")) or
            ("NDNSF_DI_PROVIDER_STAGE " in line and
             (" stage=EXECUTION_ENTERED " in line or " stage=TERMINAL " in line)))


def update(path, offsets, rows):
    if not path.exists():
        return
    with path.open("rb") as stream:
        stream.seek(offsets.get(path, 0))
        while True:
            raw = stream.readline()
            if not raw.endswith(b"\n"):
                return
            offsets[path] = stream.tell()
            line = raw.decode(errors="replace")
            if useful(line):
                rows.append(line)
                require(len(rows) <= 256, "unexpected qualification event volume")


def in_flight(requester, target, identity, role, turn_index=1):
    turns = [row["request"] for _, row in records(requester, "NATIVE_CONVERSATION_TURN_START")
             if row.get("index") == str(turn_index)]
    if not turns:
        return None
    require(len(turns) == 1, "second turn request identity is ambiguous")
    selections = [row for _, row in records(target, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED")
                  if row.get("requestId") == turns[0] and row.get("provider") == identity and
                  row.get("role") == role]
    if not selections:
        return None
    require(len(selections) == 1, "target accepted multiple selections for second turn")
    binding = {key: selections[0][key] for key in
               ("requestId", "attemptEpoch", "provider", "role", "planDigest")}
    require(int(binding["attemptEpoch"]) > 0 and re.fullmatch(r"sha256:[0-9a-f]{64}",
            binding["planDigest"]), "invalid target attempt binding")
    events = [(stamp, row) for stamp, row in records(target, "NDNSF_DI_PROVIDER_STAGE")
              if all(row.get(key) == value for key, value in binding.items())]
    if any(row.get("stage") == "TERMINAL" for _, row in events):
        raise RuntimeError("second turn already terminated before withdrawal trigger")
    entered = [stamp for stamp, row in events if row.get("stage") == "EXECUTION_ENTERED" and
               row.get("status") == "observed" and stamp is not None]
    return (binding, entered[0]) if entered else None


def require_first_turn(config, evidence, requester):
    completed = [r for _, r in records(requester, "NATIVE_CONVERSATION_TURN_COMPLETE")
                 if r.get("index") == "0"]
    require(len(completed) == 1, "first turn did not complete once")
    turn = completed[0]
    result = json.loads(Path(config["requester"]["turns"][0]["output_file"]).read_text())
    tokens = result.get("tokenIds", [])
    require(result.get("schema") == "NDNSF-DI-FINAL-V1" and
            result.get("finishReason") == "max_tokens" and len(tokens) == 32 and
            all(type(t) is int and t >= 0 for t in tokens), "first turn output mismatch")
    common = None
    for actor in config["actors"]:
        if actor["kind"] == "provider":
            attempts = {row["attemptEpoch"] for _, row in records(
                lines(evidence / (actor["id"] + ".log")), "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED")
                if row.get("requestId") == turn["request"] and row.get("planDigest") == turn["plan"]
                and row.get("provider") == actor["identity"] and int(row["attemptEpoch"]) > 0}
            common = attempts if common is None else common & attempts
    require(common and len(common) == 1, "three Providers did not complete the same first attempt")
    return turn["request"]


def denied_actor_exits(config, operation, requester_id):
    rows = operation["cleanup"]
    required = {a["id"] for a in config["actors"] if not a.get("external", False)}
    require(operation.get("status") == "STOPPED" and operation.get("failure") == "REQUESTER_EXIT:1"
            and operation["requester"]["exit_code"] == 1, "unexpected requester wait status")
    by_id = {r["id"]: r for r in rows}
    require(len(by_id) == len(rows) and required <= by_id.keys() and
            all(r.get("status") == "ZERO" and r.get("forced") is False for r in rows) and
            all(type(by_id[name].get("exitCode")) is int and
                by_id[name]["exitCode"] == (1 if name == requester_id else 0)
                for name in required), "owned actor exits missing, signalled or unexpected")
    return {name: by_id[name]["exitCode"] for name in sorted(required)}


def committed_revocation(controller, identity, service, trigger_ns):
    revoked = [(stamp, row) for stamp, row in records(controller, "NDNSF_CONTROLLER_REVOKED")
               if row.get("identity") == identity and row.get("service") == service
               and row.get("kind") == "3" and stamp is not None and stamp >= trigger_ns]
    require(len(revoked) == 1, "no unique post-trigger service revocation for the target identity")
    version = revoked[0][1]
    # The Controller's stdout trigger marker can share the logger's line.
    epoch_field = re.fullmatch(r"(\d+)(?:NDNSF_REVOCATION_TRIGGERED)?", version["epoch"])
    require(epoch_field, "invalid revoked epoch")
    epoch = epoch_field[1]
    require(any(row.get("success") == "1" and row.get("kind") == "3" and
                row.get("generation") == version["generation"] and row.get("epoch") == epoch
                for _, row in records(controller, "NDNSF_REVOCATION_APPLIED")),
            "Controller did not confirm the selected revocation")
    return version["generation"], epoch, revoked[0][0]


def verify(config, evidence, actors, binding, trigger_ns, second):
    operation = json.loads((evidence / "operation.json").read_text())
    exits = denied_actor_exits(config, operation, actors["requester"])
    service = config["network"]["service"]
    controller = lines(evidence / (actors["controller"] + ".log"))
    generation, epoch, _ = committed_revocation(controller, binding["provider"], service, trigger_ns)
    target = lines(evidence / (actors["target"] + ".log"))
    withdrawn = [stamp for stamp, row in records(target, "NDNSF_PROVIDER_AUTHORITY_WITHDRAWN")
                 if row.get("epoch") == epoch and int(row.get("lost", 0)) > 0 and
                 stamp is not None and stamp >= trigger_ns]
    require(withdrawn, "target Provider did not observe this revocation epoch")
    endings = [(stamp, row) for stamp, row in records(target, "NDNSF_DI_PROVIDER_STAGE")
               if row.get("stage") == "TERMINAL" and all(row.get(k) == v for k, v in binding.items())]
    require(len(endings) == 1 and endings[0][1].get("status") == "failed" and
            endings[0][0] is not None and endings[0][0] >= trigger_ns,
            "target attempt did not stop once after the trigger")
    marker = "NDNSF_DI_REQUEST_TERMINAL "
    terminals = [json.loads(line.split(marker, 1)[1])
                 for line in lines(evidence / (actors["requester"] + ".log")) if marker in line]
    require(len(terminals) == 1, "missing or duplicate request terminal observation")
    error = terminals[0]
    reason = error.get("message")
    # A Provider that applies the new version first can stop producing events
    # before the Requester refreshes. Require the independent Provider fence below.
    expected_reasons = {
        "Core stream failed: stream failed because the Controller authority advanced for " + service,
        "Core stream failed: stream event gap exceeded retry budget",
    }
    require(error.get("requestId") == binding["requestId"] and
            error.get("attemptEpoch") == int(binding["attemptEpoch"]) and
            error.get("code") == "NATIVE_STREAM_FAILED" and error.get("boundary") == "stream"
            and type(error.get("epochMs")) is int and error["epochMs"] >= trigger_ns // 1_000_000
            and isinstance(reason, str) and reason in expected_reasons,
            "requester terminal observation does not match the withdrawn attempt")
    require(all(not Path(second[field]).exists() for field in ("output_file", "checkpoint_output_file")),
            "revoked turn produced an output or checkpoint")
    return {"generation": generation, "epoch": epoch, "targetWithdrawnNs": withdrawn[0],
            "targetTerminalNs": endings[0][0], "cliObservedTerminalMs": error["epochMs"],
            "requesterTerminalReason": reason,
            "actorExitCodes": exits}


def target_refresh_window(target, service, period_ms):
    installed = [stamp for stamp, row in records(target, "Installed PolicyStatus")
                 if stamp and row.get("service") == service]
    if not installed:
        return False
    # Each successful native install re-arms its next scheduled refresh. Avoid
    # starting an update just after the rank has taken a full old lease.
    remaining = installed[-1] + period_ms * 1_000_000 - time.time_ns()
    return 120_000_000 <= remaining <= 350_000_000


def confirm_live_provider(evidence, target_id, binding, service, receipt, trigger_ns):
    target = lines(evidence / (target_id + ".log"))
    accepted = [t for t, row in records(target, "Installed PolicyStatus") if t and t >= trigger_ns and
                row.get("service") == service and row.get("generation") == receipt["generation"] and
                row.get("epoch") == receipt["epoch"]]
    loss = [t for t, row in records(target, "NDNSF_DI_NATIVE_PROVIDER_AUTHORITY_WITHDRAWN")
            if t and t >= receipt["targetWithdrawnNs"] and row.get("service") == service and
            row.get("provider") == binding["provider"]]
    fences = [(t, row) for t, row in records(target, "NDNSF_CONTROLLER_CACHE_INVALIDATED")
              if t and t >= trigger_ns and row.get("role") == "provider" and row.get("serviceName") == service
              and row.get("generation") == receipt["generation"] and row.get("epoch") == receipt["epoch"]]
    shutdown = [int(Decimal(line.split(maxsplit=1)[0]) * 1_000_000_000) for line in target
                if "NDNSF_DI_NATIVE_PROVIDER_SHUTDOWN_REQUESTED" in line]
    require(accepted and loss and len(fences) == 1 and len(shutdown) == 1 and
            max(accepted[0], loss[0], receipt["targetTerminalNs"]) < shutdown[0],
            "selected Provider did not observe the signed scope and stop its attempt before shutdown")
    fence_ns, fence = fences[0]
    require(fence_ns <= receipt["targetTerminalNs"] and
            int(fence["collaborationRequests"]) > 0,
            "target Provider did not invalidate active collaboration before the attempt ended")
    return {"targetSignedStatusNs": accepted[0], "targetLostServiceNs": loss[0],
            "targetShutdownNs": shutdown[0], "targetVersionFenceNs": fence_ns,
            "targetPendingKeysInvalidated": int(fence["pendingRequests"]),
            "targetCollaborationsInvalidated": int(fence["collaborationRequests"]),
            "targetFenceObservedByAttemptTerminal": fence_ns <= receipt["targetTerminalNs"]}


def verify_missing_rank(config, evidence, actors, binding, stopped, stop_ns, trigger_ns):
    operation = json.loads((evidence / "operation.json").read_text())
    exits = denied_actor_exits(config, operation, actors["requester"])
    require(stopped["status"] == "ACTOR_STOPPED" and stopped["actor_id"] == actors["target"]
            and stopped["boot_id"] == operation["boot_id"], "wrong Provider or boot stopped")
    target_exit = stopped["stop"]["result"]
    cleanup, = (r for r in operation["cleanup"] if r["id"] == actors["target"])
    require(target_exit["status"] == "ZERO" and target_exit["forced"] is False
            and target_exit["exitCode"] == 0 and target_exit["pid"] == cleanup["pid"],
            "required rank did not stop gracefully in this boot")
    target = next(a for a in config["actors"] if a["id"] == actors["controller"])["parameters"]
    generation, epoch, revoked_ns = committed_revocation(
        lines(evidence / (actors["controller"] + ".log")), target["revoke-identity"],
        target["revoke-service"], trigger_ns)
    require(revoked_ns < stop_ns and target["revoke-service"] != config["network"]["service"],
            "required rank was not serving after independent-scope rollover")
    requester = (evidence / (actors["requester"] + ".log")).read_text().splitlines()
    terminal, = (json.loads(line.partition("NDNSF_DI_REQUEST_TERMINAL ")[2]) for line in requester
                 if "NDNSF_DI_REQUEST_TERMINAL " in line)
    require(terminal["code"] == "NATIVE_STREAM_FAILED" and terminal["boundary"] == "stream"
            and terminal["requestId"] == binding["requestId"]
            and terminal["attemptEpoch"] == int(binding["attemptEpoch"])
            and terminal["epochMs"] >= stop_ns // 1_000_000,
            "missing rank did not terminate the selected stream attempt")
    delivered = [row for _, row in records(requester, "NDNSF_PHASE_TIMING")
                 if row.get("phase") == "tokenDelivered" and row.get("requestId") == binding["requestId"]]
    require(0 < len(delivered) < 32 and not any("NATIVE_CONVERSATION_TURN_COMPLETE " in l for l in requester),
            "rank loss was not observed during the incomplete first turn")
    require(all(not Path(turn[k]).exists() for turn in config["requester"]["turns"]
                for k in ("output_file", "checkpoint_output_file")), "rank loss committed output or checkpoint")
    return dict(binding, **{"generation": generation, "epoch": epoch, "actorStop": stopped,
                            "actorExitCodes": exits, "terminal": terminal, "deliveredTokens": len(delivered),
                            "stopStartedNs": stop_ns, "checkpointCommits": 0, "evidence": str(evidence)})


def run(root, config, target_role, launcher, partition=False, missing_rank=False):
    roles = config["requester"]["fixed_placement"]["role_providers"]
    identity = roles[target_role]
    actors = {a["kind"]: a["id"] for a in config["actors"] if a["kind"] in {"controller", "requester"}}
    actors["target"] = next(a["id"] for a in config["actors"] if a["identity"] == identity)
    controller = next(a for a in config["actors"] if a["id"] == actors["controller"])
    service = config["network"]["service"]
    case = "missing-rank" if missing_rank else ("partition-active" if partition else "withdrawal")
    if missing_rank:
        trigger = Path(controller["parameters"]["revoke-trigger-file"])
        trigger.touch(exist_ok=False)
        trigger_ns = trigger.stat().st_mtime_ns
        ready, release = root / "rank-token.ready", root / "rank-token.release"
        config["requester"].update(test_only=True, test_stream_barrier={
            "ready_file": str(ready), "release_file": str(release), "timeout_ms": 60000})
    else:
        trigger = root / (case + ".trigger")
        controller["parameters"].update({"revoke-identity": identity, "revoke-service": service,
                                          "revoke-attribute": "/SERVICE" + service,
                                          "revoke-trigger-file": str(trigger)})
    env = os.environ.copy()
    if partition:
        import partition_active
        partition_active.configure(config, root, controller)
        env["NDNSF_NFD_DEBUG"] = "1"
    elif not missing_rank:
        config["security"]["provider_policy_revalidation_period_ms"] = 1000
    path = root / (case + "-deployment.json")
    with path.open("x") as stream:
        json.dump(config, stream)
    second = config["requester"]["turns"][1]
    parent = Path(config["storage"]["evidence_root"]) / config["deployment_id"]
    previous = set(parent.iterdir())
    offsets, requester, target = {}, [], []
    deadline = time.monotonic() + 900
    cmd = [str(launcher), "cluster", "up", "--config", str(path), "--debug", "--timing", "--timeout", "900"]
    with (root / (case + ".stdout")).open("x") as out, (root / (case + ".stderr")).open("x") as err:
        child = subprocess.Popen(cmd, stdout=out, stderr=err, cwd=root, env=env)
        try:
            while child.poll() is None and time.monotonic() < deadline:
                boots = set(parent.iterdir()) - previous
                require(len(boots) <= 1, "concurrent boots in qualification root")
                if boots:
                    evidence = next(iter(boots))
                    update(evidence / (actors["requester"] + ".log"), offsets, requester)
                    update(evidence / (actors["target"] + ".log"), offsets, target)
                    active = in_flight(requester, target, identity, target_role, 0 if partition or missing_rank else 1)
                    if active:
                        binding, entered = active
                        if missing_rank:
                            if not ready.exists():
                                time.sleep(0.02)
                                continue
                            stop_ns = time.time_ns()
                            with (root / "missing-rank-stop.json").open("x+") as output:
                                subprocess.run([str(launcher), "cluster", "down", "--config", str(path),
                                                "--actor", actors["target"], "--timeout", "150"], cwd=root,
                                               stdout=output, stderr=err, timeout=160, check=True)
                                output.seek(0)
                                stopped = json.load(output)
                            release.touch(exist_ok=False)
                            break
                        if partition:
                            if not (root / "stream-ready").exists():
                                time.sleep(0.01)
                                continue
                            probe = partition_active.partition(root, config, evidence, trigger, binding, actors)
                            break
                        if not target_refresh_window(
                                target, service, config["security"]["provider_policy_revalidation_period_ms"]):
                            time.sleep(0.02)
                            continue
                        first = require_first_turn(config, evidence, requester)
                        require(first != binding["requestId"] and not Path(second["output_file"]).exists(),
                                "second turn has no distinct in-flight attempt")
                        trigger.touch(exist_ok=False)
                        trigger_ns = trigger.stat().st_mtime_ns
                        require(entered <= trigger_ns, "clock mismatch at probe trigger")
                        break
                time.sleep(0.05)
            else:
                raise RuntimeError("second turn execution boundary was not observed")
            require(child.wait(timeout=max(0, deadline-time.monotonic())) == 1,
                    "withdrawal deployment did not return its expected failure")
        finally:
            if child.poll() is None:
                if missing_rank:
                    release.touch(exist_ok=True)
                try:
                    subprocess.run([str(launcher), "cluster", "down", "--config", str(path)],
                                   cwd=root, stdout=err, stderr=err, timeout=90, check=True)
                    child.wait(timeout=90)
                finally:
                    if child.poll() is None:
                        child.terminate()
                        try:
                            child.wait(timeout=90)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()
                            raise RuntimeError("launcher required forced termination; inspect owned actor receipts")
    actual = json.loads((evidence / "configuration.json").read_text())
    requester = actual["requester"]
    require(actual["release"] == config["release"] and requester["model_input"]["mode"] == "stored" and
            requester["model_input"]["private_handle_file"] == config["requester"]["model_input"]["private_handle_file"],
            "installed candidate or prepared handle differs from requested deployment")
    if missing_rank:
        require(requester["fixed_placement"] == config["requester"]["fixed_placement"],
                "required role assignments changed before launch")
        return verify_missing_rank(actual, evidence, actors, binding, stopped, stop_ns, trigger_ns)
    period = config["requester"]["request"]["policy_revalidation_period_ms"]
    periods = {role: config["security"].get(role + "_policy_revalidation_period_ms", period)
               for role in ("requester", "provider")}
    require(requester["request"]["policy_revalidation_period_ms"] == period and
            all(actual["security"].get(role + "_policy_revalidation_period_ms", period) == expected
                for role, expected in periods.items()), "actual revalidation cadence differs")
    if partition:
        return partition_active.verify(actual, evidence, actors, binding, entered, probe)
    receipt = verify(config, evidence, actors, binding, trigger_ns, second)
    receipt.update(confirm_live_provider(evidence, actors["target"], binding, service, receipt, trigger_ns))
    return dict(receipt, **binding, service=service, firstTurnRequest=first, triggerNs=trigger_ns,
                executionEnteredNs=entered, roleRevalidationMs=periods, evidence=str(evidence))
