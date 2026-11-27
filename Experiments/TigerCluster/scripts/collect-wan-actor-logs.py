"""Copy stopped Slurm actor logs and node-local Provider statistics."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import sys
import tarfile
import tempfile

MANIFEST = "role-log-manifest.json"
MAX_LOG = 8 << 30
RAW_BLOCK = 1 << 20


def load_json(path):
    return json.loads(path.read_bytes())


def stopped_roles(closeout):
    close = load_json(closeout)
    boot = close["boot_id"]
    assert re.fullmatch(r"[a-f0-9]{32}", boot)
    deployment = close["deployment_id"]
    assert re.fullmatch(r"[A-Za-z0-9_-]+", deployment)
    assert close["operation_stage"] == "STOPPED" and close["actor_cleanup"] == "ZERO"
    assert close["status"] in ("COMPLETE", "FAILED")
    job = str(close["job_reference"]["job_id"])
    evidence = Path(close["evidence_root"])
    assert evidence.name == boot and evidence.parent.name == deployment
    assert evidence.resolve(strict=True) == evidence
    config = load_json(evidence / "configuration.json")
    assert config["deployment_id"] == deployment and config["platform"]["kind"] == "slurm-apptainer"
    log_root = Path(config["storage"]["actor_log_root"])
    assert log_root.is_absolute()
    allocation = load_json(evidence / "allocation.json")
    assert (allocation["boot_id"], str(allocation["job_id"])) == (boot, job)
    assert allocation["execution"] == close["execution"]
    cluster_hosts = {n["id"]: n["host"].split(".")[0] for n in config["nodes"]}
    roles = {}
    for actor in config["actors"]:
        assert not actor.get("external")
        actor_id, node = actor["id"], actor["node"]
        assert re.fullmatch(r"[A-Za-z0-9_-]+", actor_id) and actor_id not in roles
        assert re.fullmatch(r"[A-Za-z0-9_-]+", node)
        receipt = load_json(evidence / "actors" / actor_id / "receipt.json")
        host = cluster_hosts[node]
        assert (receipt["boot_id"], str(receipt["job_id"]), receipt["node_id"],
                receipt["host"].split(".")[0]) == (boot, job, node, host)
        assert receipt["configuration_sha256"] == allocation["configuration_sha256"]
        assert receipt["status"] == "STOPPED"
        launch, = [row for row in receipt["launches"] if row["id"] == actor_id]
        actual = log_root / deployment / boot / node / actor_id / (actor_id + ".log")
        assert Path(launch["log"]) == actual
        logical = f"actors/{actor_id}/runtime/{deployment}/{actor_id}.log"
        assert not (evidence / logical).exists()
        roles[actor_id] = dict(host=host, path=str(actual), member=logical)
        if actor["kind"] == "provider":
            statistics = log_root / "statistics" / (actor_id + "-statistics.jsonl")
            if actor["parameters"].get("statistics-file") == str(statistics):
                logical = f"actors/{actor_id}/runtime/{deployment}/{statistics.name}"
                assert not (evidence / logical).exists()
                roles[actor_id + ".statistics"] = dict(
                    host=host, path=str(statistics), member=logical)
    return evidence, dict(boot=boot, job=job, config=allocation["configuration_sha256"]), roles


def identify(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


class DigestReader:
    def __init__(self, stream):
        self.stream = stream
        self.sha = hashlib.sha256()
        self.total = 0

    def read(self, count):
        block = self.stream.read(count)
        self.sha.update(block)
        self.total += len(block)
        return block


def export(closeout, tar_output):
    _, identity, roles = stopped_roles(closeout)
    host = socket.gethostname().split(".")[0]
    own = {k: v for k, v in roles.items() if v["host"] == host}
    assert own, "host has no roles"
    records = {}
    with tarfile.open(fileobj=tar_output, mode="w|gz", bufsize=RAW_BLOCK) as archive:
        for key, role in own.items():
            path = Path(role["path"])
            initial = path.lstat()
            assert path.resolve(strict=True) == path
            assert stat.S_ISREG(initial.st_mode) and initial.st_uid == os.getuid()
            assert initial.st_nlink == 1 and initial.st_size <= MAX_LOG
            item = archive.gettarinfo(str(path), arcname=role["member"])
            with path.open("rb") as stream:
                assert identify(os.fstat(stream.fileno())) == identify(initial)
                digest = DigestReader(stream)
                archive.addfile(item, digest)
                assert digest.total == initial.st_size
                assert identify(os.fstat(stream.fileno())) == identify(initial)
            records[key] = dict(role, bytes=digest.total, sha256=digest.sha.hexdigest(),
                                mode=stat.S_IMODE(initial.st_mode))
        wire = json.dumps(dict(identity, host=host, logs=records), sort_keys=True).encode()
        manifest = tarfile.TarInfo(MANIFEST)
        manifest.size = len(wire)
        manifest.mode = 0o600
        manifest.uid, manifest.gid = os.getuid(), os.getgid()
        archive.addfile(manifest, io.BytesIO(wire))


def import_logs(identity, roles, paths, out):
    seen = set()
    exact = {}
    members = {info["member"]: key for key, info in roles.items()}
    for path in paths:
        rows = {}
        manifest = None
        with tarfile.open(path, mode="r|gz", bufsize=RAW_BLOCK) as archive:
            for item in archive:
                assert item.isfile() and item.uid == os.getuid() and manifest is None
                with archive.extractfile(item) as stream:
                    if item.name == MANIFEST:
                        assert 0 < item.size <= 64 << 10
                        manifest = json.load(stream)
                    else:
                        key = members[item.name]
                        assert key not in seen and key not in rows and 0 <= item.size <= MAX_LOG
                        target = out / item.name
                        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                        assert target.parent.resolve(strict=True) == target.parent
                        digest = DigestReader(stream)
                        with target.open("xb") as sink:
                            shutil.copyfileobj(digest, sink, RAW_BLOCK)
                        assert digest.total == item.size
                        target.chmod(item.mode & 0o777)
                        rows[key] = dict(roles[key], bytes=digest.total, mode=item.mode & 0o777,
                                         sha256=digest.sha.hexdigest())
        assert manifest is not None and all(identity[k] == manifest[k] for k in identity)
        assert rows and rows == manifest["logs"]
        assert {key for key, info in roles.items() if info["host"] == manifest["host"]} == set(rows)
        seen.update(rows)
        exact.update(rows)
    assert seen == set(roles)
    return exact


def replay(closeout, out, archives):
    evidence, identity, roles = stopped_roles(closeout)
    assert out.is_absolute() and not out.exists()
    with tempfile.TemporaryDirectory(prefix=".role-log-view.", dir=out.parent) as tmp:
        stage = Path(tmp) / "boot"
        shutil.copytree(evidence, stage, symlinks=True)
        exact = import_logs(identity, roles, archives, stage)
        stage.rename(out)
    print(json.dumps(dict(identity, source=str(evidence), replay=str(out), logs=exact),
                     sort_keys=True))


if __name__ == "__main__":
    cmd, argument, *args = sys.argv[1:]
    closeout = Path(argument)
    if cmd == "export":
        assert not args
        export(closeout, sys.stdout.buffer)
    elif cmd == "replay":
        replay(closeout, Path(args[0]), [Path(path) for path in args[1:]])
    else:
        raise ValueError("expected export or replay")
